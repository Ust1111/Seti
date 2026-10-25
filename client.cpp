//clang++ -Wall -o client client.cpp
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>	// UNIX
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#define PORT      7500
#define MAX_SIZE  104857600
#define MAX_NAME  255

#define ST_OK        0
#define ST_NOTFOUND  1

int sock = -1;	

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


void read_line( const char *prompt, char *out, int size )
{
	char tmp[ 2048 ];
	char *p;
	char *q;
	int len;
	int c;

	printf( "%s", prompt );
	fflush( stdout );
	if ( fgets( out, size, stdin ) == NULL )
	{
		printf( "\nВвод завершён.\n" );
		exit( 0 );
	}
	if ( strchr( out, '\n' ) == NULL )
		while ( ( c = getchar() ) != '\n' && c != EOF );
	out[ strcspn( out, "\r\n" ) ] = '\0';

	len = strlen( out );
	while ( len > 0 && strchr( " \t\"'", out[ len - 1 ] ) != NULL )
		out[ --len ] = '\0';
	p = out;
	while ( *p != '\0' && strchr( " \t\"'", *p ) != NULL )
		p++;
	memmove( out, p, strlen( p ) + 1 );

	for ( p = q = out; *p != '\0'; p++ )
	{
		if ( *p == '\\' && p[ 1 ] != '\0' )
			p++;
		*q++ = *p;
	}
	*q = '\0';

	if ( out[ 0 ] == '~' && out[ 1 ] == '/' && getenv( "HOME" ) != NULL )
	{
		snprintf( tmp, sizeof( tmp ), "%s%s", getenv( "HOME" ), out + 1 );
		snprintf( out, size, "%s", tmp );
	}
}

void read_nonempty( const char *prompt, char *out, int size )
{
	while ( 1 )
	{
		read_line( prompt, out, size );
		if ( out[ 0 ] != '\0' )
			return;
		printf( "Пустой ввод. Повторите.\n" );
	}
}

int read_port( void )
{
	char line[ 64 ];
	char *end;
	long v;
	while ( 1 )
	{
		read_line( "Порт (Enter = 7500): ", line, sizeof( line ) );
		if ( line[ 0 ] == '\0' )
			return PORT;
		v = strtol( line, &end, 10 );
		if ( *end == '\0' && v >= 1 && v <= 65535 )
			return ( int )v;
		printf( "Порт - это число от 1 до 65535.\n" );
	}
}


void close_conn( void )
{
	if ( sock >= 0 )
	{
		shutdown( sock, SHUT_WR );
		close( sock );
		sock = -1;
	}
}

void lost( void )
{
	printf( "Соединение с сервером потеряно.\n" );
	close_conn();
}

int need_conn( void )
{
	if ( sock < 0 )
	{
		printf( "Нет соединения с сервером. Сначала выберите пункт 1.\n" );
		return 0;
	}
	return 1;
}


void do_connect( void )
{
	struct sockaddr_in peer;
	struct in_addr addr;
	char line[ 1024 ];
	int port;
	int s;
	int rc;

	if ( sock >= 0 )
	{
		printf( "Соединение уже установлено. Сначала отключитесь (пункт 5).\n" );
		return;
	}

	while ( 1 )
	{
		read_nonempty( "IP-адрес сервера (например 127.0.0.1): ", line, sizeof( line ) );
		if ( inet_pton( AF_INET, line, &addr ) == 1 )
			break;
		printf( "Некорректный IPv4-адрес.\n" );
	}
	port = read_port();

	memset( &peer, 0, sizeof( peer ) );
	peer.sin_family = AF_INET;
	peer.sin_port = htons( port );
	peer.sin_addr = addr;

	s = socket( AF_INET, SOCK_STREAM, 0 );
	if ( s < 0 )
	{
		perror( "socket call failed" );
		return;
	}

	rc = connect( s, ( struct sockaddr * )&peer, sizeof( peer ) );
	if ( rc )
	{
		perror( "connect call failed" );
		close( s );
		return;
	}
	sock = s;
	printf( "Подключено.\n" );
}

void do_upload( void )
{
	char path[ 1024 ];
	char buf[ 8192 ];
	char st;
	char *name;
	struct stat sb;
	FILE *f;
	int size, rc;

	if ( !need_conn() )
		return;

	read_nonempty( "Путь к файлу для передачи: ", path, sizeof( path ) );
	if ( stat( path, &sb ) != 0 || !S_ISREG( sb.st_mode ) )
	{
		printf( "Файл не найден.\n" );
		return;
	}
	f = fopen( path, "rb" );
	if ( f == NULL )
	{
		printf( "Не удалось открыть файл для чтения.\n" );
		return;
	}
	fseek( f, 0, SEEK_END );
	size = ftell( f );
	fseek( f, 0, SEEK_SET );
	if ( size < 0 || size > MAX_SIZE )
	{
		printf( "Файл слишком большой (максимум 100 МБ).\n" );
		fclose( f );
		return;
	}
	name = strrchr( path, '/' );
	name = ( name != NULL ) ? name + 1 : path;
	if ( strlen( name ) > MAX_NAME )
	{
		printf( "Слишком длинное имя файла.\n" );
		fclose( f );
		return;
	}

	if ( !send_byte( sock, 'U' ) || !send_int( sock, strlen( name ) ) ||
	     !send_all( sock, name, strlen( name ) ) || !send_int( sock, size ) )
	{
		fclose( f );
		lost();
		return;
	}
	if ( !recv_all( sock, &st, 1 ) )
	{
		fclose( f );
		lost();
		return;
	}
	if ( st != ST_OK )
	{
		printf( "Сервер отклонил передачу файла.\n" );
		fclose( f );
		return;
	}

	while ( ( rc = fread( buf, 1, sizeof( buf ), f ) ) > 0 )
	{
		if ( !send_all( sock, buf, rc ) )
		{
			fclose( f );
			lost();
			return;
		}
	}
	fclose( f );

	if ( !recv_all( sock, &st, 1 ) )
	{
		lost();
		return;
	}
	if ( st == ST_OK )
		printf( "Файл '%s' (%d байт) передан на сервер.\n", name, size );
	else
		printf( "Сервер не подтвердил приём файла.\n" );
}

void do_download( void )
{
	char name[ 1024 ];
	char target[ 1024 ];
	char full[ 2048 ];
	char parent[ 2048 ];
	char answer[ 16 ];
	char buf[ 8192 ];
	char st;
	char *slash;
	struct stat sb;
	FILE *f;
	int size, left, want, rc;

	if ( !need_conn() )
		return;

	read_nonempty( "Имя файла на сервере: ", name, sizeof( name ) );
	if ( strchr( name, '/' ) != NULL )
	{
		printf( "Укажите только имя файла, без пути.\n" );
		return;
	}
	if ( strlen( name ) > MAX_NAME )
	{
		printf( "Слишком длинное имя файла.\n" );
		return;
	}
	read_nonempty( "Куда сохранить (папка или полный путь к файлу): ", target, sizeof( target ) );

	if ( stat( target, &sb ) == 0 && S_ISDIR( sb.st_mode ) )
		snprintf( full, sizeof( full ), "%s/%s", target, name );
	else
		snprintf( full, sizeof( full ), "%s", target );

	snprintf( parent, sizeof( parent ), "%s", full );
	slash = strrchr( parent, '/' );
	if ( slash == NULL )
		snprintf( parent, sizeof( parent ), "." );
	else if ( slash == parent )
		snprintf( parent, sizeof( parent ), "/" );
	else
		*slash = '\0';
	if ( stat( parent, &sb ) != 0 || !S_ISDIR( sb.st_mode ) )
	{
		printf( "Папка назначения не существует.\n" );
		return;
	}
	if ( stat( full, &sb ) == 0 )
	{
		read_line( "Такой файл уже есть. Перезаписать? (y/n): ", answer, sizeof( answer ) );
		if ( strcmp( answer, "y" ) != 0 && strcmp( answer, "Y" ) != 0 )
		{
			printf( "Отменено.\n" );
			return;
		}
	}

	if ( !send_byte( sock, 'D' ) || !send_int( sock, strlen( name ) ) ||
	     !send_all( sock, name, strlen( name ) ) )
	{
		lost();
		return;
	}
	if ( !recv_all( sock, &st, 1 ) )
	{
		lost();
		return;
	}
	if ( st == ST_NOTFOUND )
	{
		printf( "На сервере нет такого файла.\n" );
		return;
	}
	if ( st != ST_OK )
	{
		printf( "Сервер отклонил запрос.\n" );
		return;
	}
	if ( !recv_int( sock, &size ) )
	{
		lost();
		return;
	}
	if ( size < 0 || size > MAX_SIZE )
	{
		printf( "Получен некорректный размер файла.\n" );
		close_conn();
		return;
	}

    
	f = fopen( full, "wb" );
	left = size;
	while ( left > 0 )
	{
		want = left < ( int )sizeof( buf ) ? left : ( int )sizeof( buf );
		rc = recv( sock, buf, want, 0 );
		if ( rc <= 0 )
		{
			if ( f != NULL )
			{
				fclose( f );
				remove( full );
			}
			lost();
			return;
		}
		if ( f != NULL )
			fwrite( buf, 1, rc, f );
		left -= rc;
	}
	if ( f == NULL )
	{
		printf( "Не удалось создать файл (нет прав?). Данные от сервера отброшены.\n" );
		return;
	}
	fclose( f );
	printf( "Файл сохранён: %s (%d байт).\n", full, size );
}

void do_list( void )
{
	char text[ 65537 ];
	char st;
	int len;

	if ( !need_conn() )
		return;
	if ( !send_byte( sock, 'L' ) )
	{
		lost();
		return;
	}
	if ( !recv_all( sock, &st, 1 ) || !recv_int( sock, &len ) )
	{
		lost();
		return;
	}
	if ( st != ST_OK || len < 0 || len > 65536 )
	{
		printf( "Ошибка сервера.\n" );
		close_conn();
		return;
	}
	if ( len > 0 && !recv_all( sock, text, len ) )
	{
		lost();
		return;
	}
	text[ len ] = '\0';
	if ( len == 0 )
		printf( "На сервере нет файлов.\n" );
	else
		printf( "Файлы на сервере:\n%s", text );
}

void do_disconnect( void )
{
	if ( sock < 0 )
	{
		printf( "Соединения нет.\n" );
		return;
	}
	send_byte( sock, 'Q' );	
	close_conn();
	printf( "Отключено.\n" );
}

int main( void )
{
	char choice[ 64 ];

	signal( SIGPIPE, SIG_IGN );

	while ( 1 )
	{
		printf( "\n Файловый клиент \n" );
		printf( sock >= 0 ? "Состояние: подключён\n" : "Состояние: нет соединения\n" );
		printf( "1 - Подключиться к серверу\n" );
		printf( "2 - Передать файл на сервер\n" );
		printf( "3 - Прочитать (скачать) файл с сервера\n" );
		printf( "4 - Список файлов на сервере\n" );
		printf( "5 - Отключиться\n" );
		printf( "0 - Выход\n" );
		read_line( "Ваш выбор: ", choice, sizeof( choice ) );

		if ( strcmp( choice, "1" ) == 0 )
			do_connect();
		else if ( strcmp( choice, "2" ) == 0 )
			do_upload();
		else if ( strcmp( choice, "3" ) == 0 )
			do_download();
		else if ( strcmp( choice, "4" ) == 0 )
			do_list();
		else if ( strcmp( choice, "5" ) == 0 )
			do_disconnect();
		else if ( strcmp( choice, "0" ) == 0 )
		{
			do_disconnect();
			return 0;
		}
		else
			printf( "Неверный пункт меню. Введите число от 0 до 5.\n" );
	}
}