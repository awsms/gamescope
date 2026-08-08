#include "InputLatencyTrace.hpp"

#include "log.hpp"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <thread>
#include <time.h>
#include <unistd.h>

static LogScope input_latency_log( "input_latency" );

namespace
{
	constexpr uint32_t kMagic = 0x47534954u;
	constexpr uint32_t kVersion = 1u;
	constexpr uint32_t kMessageHello = 1u;
	constexpr uint32_t kMessageHelloOk = 2u;
	constexpr uint32_t kMessageArm = 4u;
	constexpr uint32_t kMessageArmed = 5u;
	constexpr uint32_t kMessageRecord = 6u;
	constexpr uint32_t kMessageStop = 8u;
	constexpr uint32_t kRoleGamescope = 6u;
	constexpr uint32_t kEventKey = 1u;
	constexpr uint32_t kFlagPress = 1u << 1;

	struct TraceMessage
	{
		uint32_t magic;
		uint32_t version;
		uint32_t type;
		uint32_t role;
		uint32_t stage;
		uint32_t eventType;
		uint64_t sequence;
		uint64_t timestampNs;
		int32_t code;
		int32_t value;
		uint32_t pid;
		uint32_t tid;
		uint32_t flags;
		uint32_t reserved;
	};

	static_assert( sizeof( TraceMessage ) == 64 );

	struct PendingKeyTrace
	{
		uint64_t sequence;
		uint64_t timestamps[4];
		uint32_t key;
		uint32_t flags;
	};

	std::atomic<int> s_socket{ -1 };
	std::atomic<bool> s_running{ false };
	std::atomic<bool> s_enabled{ false };
	std::atomic<uint64_t> s_sequence{ 0 };
	std::atomic<int32_t> s_code{ 0 };
	std::atomic<uint32_t> s_flags{ 0 };
	std::mutex s_sendMutex;
	std::thread s_controlThread;

	uint32_t GetTid()
	{
		return uint32_t( syscall( SYS_gettid ) );
	}

	uint64_t GetTimeNs()
	{
		timespec timestamp{};
		if ( clock_gettime( CLOCK_MONOTONIC, &timestamp ) != 0 )
			return 0;
		return uint64_t( timestamp.tv_sec ) * 1000000000ull +
			uint64_t( timestamp.tv_nsec );
	}

	TraceMessage MakeMessage( uint32_t type )
	{
		TraceMessage message{};
		message.magic = kMagic;
		message.version = kVersion;
		message.type = type;
		message.role = kRoleGamescope;
		message.pid = uint32_t( getpid() );
		message.tid = GetTid();
		return message;
	}

	bool SendMessage( const TraceMessage &message )
	{
		std::scoped_lock lock( s_sendMutex );
		const int socket = s_socket.load( std::memory_order_acquire );
		if ( socket < 0 )
			return false;

		ssize_t result;
		do
		{
			result = send( socket, &message, sizeof( message ), MSG_NOSIGNAL );
		} while ( result < 0 && errno == EINTR );
		return result == ssize_t( sizeof( message ) );
	}

	bool ReceiveMessage( int socket, TraceMessage *message )
	{
		ssize_t result;
		do
		{
			result = recv( socket, message, sizeof( *message ), 0 );
		} while ( result < 0 && errno == EINTR );

		return result == ssize_t( sizeof( *message ) ) &&
			message->magic == kMagic && message->version == kVersion;
	}

	int ConnectSocket( const char *path )
	{
		sockaddr_un address{};
		const size_t pathLength = strlen( path );
		if ( pathLength == 0 || pathLength >= sizeof( address.sun_path ) )
		{
			errno = ENAMETOOLONG;
			return -1;
		}

		const int socket = ::socket( AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0 );
		if ( socket < 0 )
			return -1;
		address.sun_family = AF_UNIX;
		memcpy( address.sun_path, path, pathLength + 1 );
		if ( connect( socket, reinterpret_cast<sockaddr *>( &address ),
				sizeof( address ) ) != 0 )
		{
			const int savedErrno = errno;
			close( socket );
			errno = savedErrno;
			return -1;
		}
		return socket;
	}

	void ControlThreadMain()
	{
		pthread_setname_np( pthread_self(), "gamescope-itrace" );
		const int socket = s_socket.load( std::memory_order_acquire );

		while ( s_running.load( std::memory_order_acquire ) )
		{
			TraceMessage message{};
			if ( !ReceiveMessage( socket, &message ) )
				break;
			if ( message.type == kMessageStop )
				break;
			if ( message.type != kMessageArm ||
				message.eventType != kEventKey || message.sequence == 0 )
				continue;

			s_sequence.store( 0, std::memory_order_release );
			s_code.store( message.code, std::memory_order_relaxed );
			s_flags.store( message.flags, std::memory_order_relaxed );
			s_sequence.store( message.sequence, std::memory_order_release );

			TraceMessage armed = MakeMessage( kMessageArmed );
			armed.sequence = message.sequence;
			if ( !SendMessage( armed ) )
				break;
		}

		s_enabled.store( false, std::memory_order_release );
		s_running.store( false, std::memory_order_release );
	}
}

void input_latency_trace_init()
{
	const char *path = getenv( "GAMESCOPE_INPUT_LATENCY_TRACE_SOCKET" );
	if ( path == nullptr || path[0] == '\0' )
		return;

	const int socket = ConnectSocket( path );
	if ( socket < 0 )
	{
		input_latency_log.errorf_errno( "Failed to connect to collector at '%s'", path );
		return;
	}
	s_socket.store( socket, std::memory_order_release );

	TraceMessage hello = MakeMessage( kMessageHello );
	if ( !SendMessage( hello ) )
	{
		input_latency_log.errorf_errno( "Failed to register with collector" );
		input_latency_trace_shutdown();
		return;
	}

	pollfd pollFd{ .fd = socket, .events = POLLIN };
	TraceMessage response{};
	if ( poll( &pollFd, 1, 5000 ) != 1 || !ReceiveMessage( socket, &response ) ||
		response.type != kMessageHelloOk )
	{
		input_latency_log.errorf( "Collector did not acknowledge Gamescope trace registration." );
		input_latency_trace_shutdown();
		return;
	}

	s_running.store( true, std::memory_order_release );
	s_enabled.store( true, std::memory_order_release );
	s_controlThread = std::thread( ControlThreadMain );
	input_latency_log.infof( "Connected to collector at '%s'.", path );
}

void input_latency_trace_shutdown()
{
	s_enabled.store( false, std::memory_order_release );
	s_running.store( false, std::memory_order_release );
	const int socket = s_socket.exchange( -1, std::memory_order_acq_rel );
	if ( socket >= 0 )
		::shutdown( socket, SHUT_RDWR );
	if ( s_controlThread.joinable() )
		s_controlThread.join();
	if ( socket >= 0 )
		close( socket );
	s_sequence.store( 0, std::memory_order_release );
}

void input_latency_trace_record_key( InputLatencyTraceStage stage,
	uint32_t key, bool pressed )
{
	thread_local PendingKeyTrace pending{};

	if ( !pressed || !s_enabled.load( std::memory_order_acquire ) )
		return;
	const uint64_t sequence = s_sequence.load( std::memory_order_acquire );
	if ( sequence == 0 || int32_t( key ) != s_code.load( std::memory_order_relaxed ) )
		return;

	if ( stage == INPUT_LATENCY_T2_BACKEND )
	{
		pending = {};
		pending.sequence = sequence;
		pending.key = key;
		pending.flags = s_flags.load( std::memory_order_relaxed ) | kFlagPress;
	}
	else if ( pending.sequence != sequence || pending.key != key )
	{
		return;
	}

	const uint32_t timestampIndex = uint32_t( stage ) - uint32_t( INPUT_LATENCY_T2_BACKEND );
	if ( timestampIndex >= 4 )
		return;
	pending.timestamps[timestampIndex] = GetTimeNs();
	if ( stage != INPUT_LATENCY_T5_FLUSH )
		return;

	for ( uint32_t index = 0; index < 4; index++ )
	{
		if ( pending.timestamps[index] == 0 )
			continue;
		TraceMessage record = MakeMessage( kMessageRecord );
		record.stage = uint32_t( INPUT_LATENCY_T2_BACKEND ) + index;
		record.eventType = kEventKey;
		record.sequence = pending.sequence;
		record.timestampNs = pending.timestamps[index];
		record.code = int32_t( pending.key );
		record.value = 1;
		record.flags = pending.flags;
		if ( !SendMessage( record ) )
		{
			s_enabled.store( false, std::memory_order_release );
			break;
		}
	}
	pending = {};
}
