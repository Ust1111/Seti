//clang++ -Wall -o server server.cpp
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <sys/socket.h>	// UNIX
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define PORT      7500        
#define MAX_SIZE  104857600     //100 МБ
#define MAX_NAME  255      
#define DIR_NAME  "server_files" 


#define ST_OK        0
#define ST_NOTFOUND  1
#define ST_BAD       2


int recv_all( int s, char *buf, int n )
{
	int got = 0;
	int rc;
	while ( got < n )
	{
		rc = recv( s, buf + got, n - got, 0 );
		if ( rc <= 0 )
			return 0;
		got += rc;
	}
	return 1;
}

int send_all( int s, const char *buf, int n )
{
	int sent = 0;
	int rc;
	while ( sent < n )
	{
		rc = send( s, buf + sent, n - sent, 0 );
		if ( rc <= 0 )
			return 0;
		sent += rc;
	}
	return 1;
}

int send_byte( int s, char c )
{
	return send_all( s, &c, 1 );
}

int send_int( int s, int v )
{
	unsigned int net = htonl( v );
	return send_all( s, ( char * )&net, 4 );
}

int recv_int( int s, int *v )
{
	unsigned int net;
	if ( !recv_all( s, ( char * )&net, 4 ) )
		return 0;
	*v = ntohl( net );
	return 1;
}

int recv_name( int s, char *name )
{
	int len;
	if ( !recv_int( s, &len ) || len <= 0 || len > MAX_NAME )
		return 0;
	if ( !recv_all( s, name, len ) )
		return 0;
	name[ len ] = '\0';
	if ( strlen( name ) != ( size_t )len )
		return 0;
	return 1;
}

int name_ok( const char *name )
{
	if ( name[ 0 ] == '.' )
		return 0;
	if ( strchr( name, '/' ) != NULL || strchr( name, '\\' ) != NULL )
		return 0;
	return 1;
}

int do_upload( int s )
{
	char name[ MAX_NAME + 1 ];
	char path[ 512 ];
	char buf[ 8192 ];
	int size, left, want, rc;
	FILE *f;

	if ( !recv_name( s, name ) || !recv_int( s, &size ) )
		return 0;
	if ( !name_ok( name ) || size < 0 || size > MAX_SIZE )
		return send_byte( s, ST_BAD );

	snprintf( path, sizeof( path ), "%s/%s", DIR_NAME, name );
	f = fopen( path, "wb" );
	if ( f == NULL )
		return send_byte( s, ST_BAD );

	if ( !send_byte( s, ST_OK ) )
	{
		fclose( f );
		return 0;
	}

	left = size;
	while ( left > 0 )
	{
		want = left < ( int )sizeof( buf ) ? left : ( int )sizeof( buf );
		rc = recv( s, buf, want, 0 );
		if ( rc <= 0 || fwrite( buf, 1, rc, f ) != ( size_t )rc )
		{
			fclose( f );
			remove( path );	
			return 0;
		}
		left -= rc;
	}
	fclose( f );
	printf( "    Получен файл '%s', %d байт\n", name, size );
	return send_byte( s, ST_OK );
}

int do_download( int s )
{
	char name[ MAX_NAME + 1 ];
	char path[ 512 ];
	char buf[ 8192 ];
	int size, rc;
	FILE *f;

	if ( !recv_name( s, name ) )
		return 0;
	if ( !name_ok( name ) )
		return send_byte( s, ST_BAD );

	snprintf( path, sizeof( path ), "%s/%s", DIR_NAME, name );
	f = fopen( path, "rb" );
	if ( f == NULL )
		return send_byte( s, ST_NOTFOUND );

	fseek( f, 0, SEEK_END );
	size = ftell( f );
	fseek( f, 0, SEEK_SET );
	if ( size < 0 || size > MAX_SIZE )
	{
		fclose( f );
		return send_byte( s, ST_BAD );
	}

	if ( !send_byte( s, ST_OK ) || !send_int( s, size ) )
	{
		fclose( f );
		return 0;
	}
	while ( ( rc = fread( buf, 1, sizeof( buf ), f ) ) > 0 )
	{
		if ( !send_all( s, buf, rc ) )
		{
			fclose( f );
			return 0;
		}
	}
	fclose( f );
	printf( "    Отправлен файл '%s', %d байт\n", name, size );
	return 1;
}

int do_list( int s )
{
	char text[ 65536 ];
	char path[ 512 ];
	int len = 0;
	DIR *d;
	struct dirent *e;
	struct stat st;

	d = opendir( DIR_NAME );
	if ( d != NULL )
	{
		while ( ( e = readdir( d ) ) != NULL )
		{
			if ( e->d_name[ 0 ] == '.' )
				continue;
			snprintf( path, sizeof( path ), "%s/%s", DIR_NAME, e->d_name );
			if ( stat( path, &st ) != 0 || !S_ISREG( st.st_mode ) )
				continue;
			if ( len > ( int )sizeof( text ) - 400 )
				break;
			len += snprintf( text + len, sizeof( text ) - len, "%s (%ld байт)\n", e->d_name, ( long )st.st_size );
		}
		closedir( d );
	}
	return send_byte( s, ST_OK ) && send_int( s, len ) && send_all( s, text, len );
}

void serve( int s1 )
{
	char cmd;
	int ok = 1;

	while ( ok && recv_all( s1, &cmd, 1 ) )
	{
		if ( cmd == 'U' )
			ok = do_upload( s1 );
		else if ( cmd == 'D' )
			ok = do_download( s1 );
		else if ( cmd == 'L' )
			ok = do_list( s1 );
		else if ( cmd == 'Q' )
			break;			
		else
		{
			send_byte( s1, ST_BAD );
			break;
		}
	}
}

int main( int argc, char *argv[] )
{
	struct sockaddr_in local;
	struct sockaddr_in peer;
	socklen_t peer_len;
	int s;
	int s1;
	int rc;
	int port = PORT;
	int yes = 1;

	signal( SIGPIPE, SIG_IGN );

	if ( argc > 1 )	
	{
		char *end;
		long v = strtol( argv[ 1 ], &end, 10 );
		if ( *end != '\0' || v < 1 || v > 65535 )
		{
			printf( "Использование: ./server [порт 1-65535]\n" );
			exit( 1 );
		}
		port = ( int )v;
	}

	mkdir( DIR_NAME, 0777 );

	memset( &local, 0, sizeof( local ) );
	local.sin_family = AF_INET;
	local.sin_port = htons( port );	
	local.sin_addr.s_addr = htonl( INADDR_ANY );

	s = socket( AF_INET, SOCK_STREAM, 0 );
	if ( s < 0 )
	{
		perror( "socket call failed" );
		exit( 1 );
	}

	setsockopt( s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof( yes ) );

	rc = bind( s, ( struct sockaddr * )&local, sizeof( local ) );
	if ( rc < 0 )
	{
		perror( "bind call failure" );
		exit( 1 );
	}

	rc = listen( s, 5 );
	if ( rc )
	{
		perror( "listen call failed" );
		exit( 1 );
	}

	printf( "Сервер слушает порт %d. Файлы хранятся в папке '%s'.\n", port, DIR_NAME );
	printf( "Остановить сервер: Ctrl+C\n" );

	while ( 1 )
	{
		peer_len = sizeof( peer );
		s1 = accept( s, ( struct sockaddr * )&peer, &peer_len );
		if ( s1 < 0 )
		{
			perror( "accept call failed" );
			continue;
		}
		printf( "[+] Подключился клиент %s:%d\n", inet_ntoa( peer.sin_addr ), ntohs( peer.sin_port ) );

		serve( s1 );

		shutdown( s1, SHUT_WR );
		close( s1 );
		printf( "[-] Соединение закрыто\n" );
	}
}