#include "WindowInspector.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_set>

namespace gamescope
{
    namespace
    {
        int g_xError = 0;

        int HandleXError( Display *, XErrorEvent *event )
        {
            g_xError = event->error_code;
            return 0;
        }

        std::string EscapeField( std::string_view input )
        {
            std::string output;
            output.reserve( input.size() );
            for ( char ch : input )
            {
                switch ( ch )
                {
                    case '\\': output += "\\\\"; break;
                    case '\t': output += "\\t"; break;
                    case '\n': output += "\\n"; break;
                    case '\r': output += "\\r"; break;
                    case '\0': output += "\\0"; break;
                    default: output += ch; break;
                }
            }
            return output;
        }

        struct WindowInspector
        {
            Display *display;
            Window root;
            Atom utf8Name;
            Atom pidAtom;
            Atom wmName;
            Atom wmClass;
            std::unordered_set<Window> tracked;
            std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

            void Print( const char *event, Window window ) const
            {
                XWindowAttributes attributes{};
                g_xError = 0;
                const bool hasAttributes = XGetWindowAttributes( display, window, &attributes );
                XSync( display, False );
                const bool exists = hasAttributes && !g_xError;

                std::string title;
                std::string windowClass;
                unsigned long pid = 0;
                if ( exists )
                {
                    Atom actualType = None;
                    int actualFormat = 0;
                    unsigned long count = 0;
                    unsigned long remaining = 0;
                    unsigned char *value = nullptr;
                    if ( XGetWindowProperty( display, window, utf8Name, 0, 1024, False,
                                             AnyPropertyType, &actualType, &actualFormat,
                                             &count, &remaining, &value ) == Success &&
                         actualFormat == 8 && value )
                    {
                        title.assign( reinterpret_cast<const char *>( value ), count );
                    }
                    if ( value ) XFree( value );

                    if ( title.empty() )
                    {
                        char *legacyName = nullptr;
                        if ( XFetchName( display, window, &legacyName ) && legacyName )
                            title = legacyName;
                        if ( legacyName ) XFree( legacyName );
                    }

                    XClassHint hint{};
                    if ( XGetClassHint( display, window, &hint ) )
                    {
                        if ( hint.res_name ) windowClass = hint.res_name;
                        if ( hint.res_class )
                        {
                            if ( !windowClass.empty() ) windowClass += '/';
                            windowClass += hint.res_class;
                        }
                        if ( hint.res_name ) XFree( hint.res_name );
                        if ( hint.res_class ) XFree( hint.res_class );
                    }

                    value = nullptr;
                    if ( XGetWindowProperty( display, window, pidAtom, 0, 1, False,
                                             XA_CARDINAL, &actualType, &actualFormat,
                                             &count, &remaining, &value ) == Success &&
                         actualFormat == 32 && count == 1 && value )
                    {
                        pid = *reinterpret_cast<unsigned long *>( value );
                    }
                    if ( value ) XFree( value );
                    XSync( display, False );
                }

                const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started ).count();
                const std::string safeClass = EscapeField( windowClass );
                const std::string safeTitle = EscapeField( title );
                std::printf( "%s\t%lld\t0x%lx\t%s\t%lu\t%s\t%s\n", event,
                             static_cast<long long>( elapsed ), window,
                             exists ? ( attributes.map_state == IsUnmapped ? "0" : "1" ) : "?",
                             pid, safeClass.c_str(), safeTitle.c_str() );
                std::fflush( stdout );
            }

            void Track( Window window )
            {
                if ( !tracked.insert( window ).second ) return;
                g_xError = 0;
                XSelectInput( display, window, PropertyChangeMask );
                XSync( display, False );
                if ( g_xError ) tracked.erase( window );
            }

            bool Snapshot( const char *event, bool trackWindows )
            {
                Window returnedRoot = None;
                Window parent = None;
                Window *children = nullptr;
                unsigned int count = 0;
                if ( !XQueryTree( display, root, &returnedRoot, &parent, &children, &count ) )
                    return false;
                for ( unsigned int i = 0; i < count; ++i )
                {
                    if ( trackWindows ) Track( children[i] );
                    Print( event, children[i] );
                }
                if ( children ) XFree( children );
                return true;
            }

            bool Watch()
            {
                g_xError = 0;
                XSelectInput( display, root, SubstructureNotifyMask );
                XSync( display, False );
                if ( g_xError )
                {
                    std::fprintf( stderr, "Could not subscribe to Xwayland window events.\n" );
                    return false;
                }
                std::puts( "event\ttime_us\tid\tmapped\tpid\tclass\ttitle" );
                if ( !Snapshot( "existing", true ) ) return false;
                std::fflush( stdout );

                for ( ;; )
                {
                    XEvent event{};
                    XNextEvent( display, &event );
                    switch ( event.type )
                    {
                        case CreateNotify:
                            if ( event.xcreatewindow.parent == root )
                            {
                                Track( event.xcreatewindow.window );
                                Print( "create", event.xcreatewindow.window );
                            }
                            break;
                        case MapNotify:
                            if ( tracked.contains( event.xmap.window ) )
                                Print( "map", event.xmap.window );
                            break;
                        case UnmapNotify:
                            if ( tracked.contains( event.xunmap.window ) )
                                Print( "unmap", event.xunmap.window );
                            break;
                        case DestroyNotify:
                            if ( tracked.erase( event.xdestroywindow.window ) )
                                Print( "destroy", event.xdestroywindow.window );
                            break;
                        case PropertyNotify:
                            if ( tracked.contains( event.xproperty.window ) &&
                                 ( event.xproperty.atom == utf8Name ||
                                   event.xproperty.atom == pidAtom ||
                                   event.xproperty.atom == wmName ||
                                   event.xproperty.atom == wmClass ) )
                                Print( "property", event.xproperty.window );
                            break;
                        default:
                            break;
                    }
                }
                return true;
            }
        };
    }

    int RunWindowInspection( std::string_view command, const char *displayName )
    {
        if ( !displayName || !*displayName )
        {
            std::fprintf( stderr, "No Gamescope Xwayland display selected. Run inside Gamescope or pass an X display.\n" );
            return 1;
        }
        Display *display = XOpenDisplay( displayName );
        if ( !display )
        {
            std::fprintf( stderr, "Could not connect to the selected Xwayland display.\n" );
            return 1;
        }

        XSetErrorHandler( HandleXError );
        WindowInspector inspector{
            .display = display,
            .root = DefaultRootWindow( display ),
            .utf8Name = XInternAtom( display, "_NET_WM_NAME", False ),
            .pidAtom = XInternAtom( display, "_NET_WM_PID", False ),
            .wmName = XInternAtom( display, "WM_NAME", False ),
            .wmClass = XInternAtom( display, "WM_CLASS", False ),
            .tracked = {},
            .started = std::chrono::steady_clock::now(),
        };

        if ( command == "watch-windows" )
        {
            return inspector.Watch() ? 0 : 1;
        }
        std::puts( "event\ttime_us\tid\tmapped\tpid\tclass\ttitle" );
        const bool success = inspector.Snapshot( "current", false );
        XCloseDisplay( display );
        return success ? 0 : 1;
    }
}
