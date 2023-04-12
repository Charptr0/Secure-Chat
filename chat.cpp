#include <curses.h>
#include <readline/history.h>
#include <readline/readline.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/bio.h> // encode to base64
#include <openssl/buffer.h>
#include <string.h>
#include <getopt.h>
#include <string>
using std::string;
#include <deque>
using std::deque;
#include <pthread.h>
#include <utility>
using std::pair;
#include "dh.h"

static pthread_t trecv;     /* wait for incoming messagess and post to queue */
void* recvMsg(void*);       /* for trecv */
static pthread_t tcurses;   /* setup curses and draw messages from queue */
void* cursesthread(void*);  /* for tcurses */
/* tcurses will get a queue full of these and redraw the appropriate windows */
struct redraw_data {
	bool resize;
	string msg;
	string sender;
	WINDOW* win;
};
static deque<redraw_data> mq; /* messages and resizes yet to be drawn */
/* manage access to message queue: */
static pthread_mutex_t qmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcv = PTHREAD_COND_INITIALIZER;

/* XXX different colors for different senders */

/* record chat history as deque of strings: */
static deque<string> transcript;

#define max(a, b)         \
	({ typeof(a) _a = a;    \
	 typeof(b) _b = b;    \
	 _a > _b ? _a : _b; })

/* network stuff... */

int listensock, sockfd;

static bool should_exit = false;

// global variables for pks and sks
mpz_t global_user1_pk;
mpz_t global_user1_sk;
mpz_t global_user2_pk;
mpz_t global_user2_sk;

const size_t klen = 128;
int base = 62; //why? idk. refer to https://gmplib.org/manual/I_002fO-of-Integers#I_002fO-of-Integers
bool isclient; //turned global for convienence
bool gotPK = false;
bool varification = false;
unsigned char kA[klen]; //client dhfinal
unsigned char kB[klen]; //server dhfinal

/**
 * Write a log message to a text file
 * @author Chenhao L.
*/
int log(const char* message, const char* filename = "log.txt") {
    
    // check to make sure file name exists
    FILE *logFile = fopen(filename, "r");
    if(logFile == NULL) {
        // create this file
        FILE *newLogFile = fopen(filename, "w");
        
        if(newLogFile == NULL)  {
            printf("Cannot create %s", filename); 
            return 1;
        }

        fprintf(newLogFile, "--Beginning of the log file--\n");

        fclose(newLogFile);
    } else fclose(logFile);

    // write to log file
    FILE *fp;
	fp = fopen(filename, "a");
	if(fp == NULL) {
        printf("Cannot open %s", filename);
		return 1; 
	}

	fprintf(fp, "%s\n", message);
	fclose(fp);

    return 0;
}

/**
 * Log the encrypted message in bytes to log.txt
 * @author Chenhao L.
*/
void logEncryptedMessage(unsigned char encryptedMessage[64]) {
	log("Logging the encrypted message in bytes...");
	
	for(size_t i = 0; i < 64; i++) {
		char* text = (char*)malloc(3 * sizeof(char));
		sprintf(text, "%02x", encryptedMessage[i]);
		log(text);
		free(text);
	}

	log("Finished logging the encrypted message in bytes");
}

/**
 * Log the encrypted message encoded in base64 to log.txt
 * @author Chenhao L.
*/
void logEncryptedMessage(const char* encryptedMessage) {
	log("Logging the encrypted message in base 64");
	log(encryptedMessage);
	log("Finished logging the encrypted message in base 64");
}

/**
 * Encrypt a string using HMAC encryption
 * @param message - The string to encrypt
 * @param key - the secret key of the current user
 * @return the encrypted message encoded in base64
 * @author Chenhao L.
*/
char* encryptMessage(const char* message, const char* key) {
	unsigned char mac[64];
	
	size_t msg_len = strlen(message);
	size_t key_len = strlen(key);

	// add HMAC encryption
	memset(mac,0,64);
	HMAC(EVP_sha512(),key,key_len,(unsigned char*)message, msg_len,mac,0);

	logEncryptedMessage(mac);

	// encode the string into base64
	BIO* bio = BIO_new(BIO_s_mem());
	BIO* base64 = BIO_new(BIO_f_base64());

	// wtf is this
	BIO_push(base64, bio);
	BIO_write(base64, mac, 64);
	BIO_flush(base64);

	BUF_MEM* mem = NULL;
	BIO_get_mem_ptr(bio, &mem);
	char* base64_mac = (char*)malloc(mem->length + 1);
	memcpy(base64_mac, mem->data, mem->length);
	base64_mac[mem->length] = '\0';
	BIO_free_all(base64);

	logEncryptedMessage(base64_mac);
	// encode bytes into b64
	return base64_mac;
}

/**
 * Decrypt the encoded message with the corresponding key
 * @author Chenhao L.
 * @author ???
*/
char* decryptMessage(const char* encodedMessage, const char* key) {
	size_t encodedMessage_len = strlen(encodedMessage);
	logEncryptedMessage(encodedMessage);

	// decode the encoded b64 string back into bytes
	BIO* bio = BIO_new_mem_buf(encodedMessage, encodedMessage_len);
	BIO* base64 = BIO_new(BIO_f_base64());
	BIO_push(base64, bio);

	unsigned char encodedMessageInBytes[64];
	BIO_read(base64, encodedMessageInBytes, encodedMessage_len);
	BIO_free_all(base64);

	// now the message should be in bytes instead of b64
	// decryption happens here

	logEncryptedMessage(encodedMessageInBytes);

	return NULL;
}

[[noreturn]] static void fail_exit(const char *msg);

[[noreturn]] static void error(const char *msg)
{
	perror(msg);
	fail_exit("");
}

// required handshake with the client
int initServerNet(int port)
{
	int reuse = 1;
	struct sockaddr_in serv_addr;
	listensock = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(listensock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	/* NOTE: might not need the above if you make sure the client closes first */
	if (listensock < 0)
		error("ERROR opening socket");
	bzero((char *) &serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(port);
	if (bind(listensock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
		error("ERROR on binding");
	fprintf(stderr, "listening on port %i...\n",port);
	listen(listensock,1);
	socklen_t clilen;
	struct sockaddr_in  cli_addr;
	sockfd = accept(listensock, (struct sockaddr *) &cli_addr, &clilen);
	if (sockfd < 0)
		error("error on accept");
	close(listensock);
	fprintf(stderr, "connection made, starting session...\n");
	/* at this point, should be able to send/recv on sockfd */
	log("initServerNet: Successfully connected to server");
	
	if (init("params") != 0) {
		log("initServerNet: Cannot init Diffie Hellman key exchange :(");
		printf("Cannot init Diffie Hellman key exchange :(");

		should_exit = true;

		// instead of shutting down the program when keys cannot be generated, in the future
		// there should be a feature that informs both users that the chat is not encrypted and unsecured
		// - Chenhao L.
	}

	// generate user 2 key
	NEWZ(global_user2_sk);
	NEWZ(global_user2_pk);
	if(dhGen(global_user2_sk, global_user2_pk) != 0) {
		log("Something went wrong in dhGen() on the server, did you run the init() function?");

		// exit the program
		should_exit = true;
	}

	//store public key 2 (g^a mod p) to file "PublicKeyServer". 
	FILE *pk2 = fopen("PublicKeyServer", "w");
	mpz_out_str(pk2, base, global_user2_pk);
	fclose(pk2);

	// //read from file to get other persons public key 1.
	// FILE *pk1 = fopen("PublicKeyClient", "r");
	// mpz_inp_str(global_user1_pk, pk1, base);
	// fclose(pk1);
	// error: dhfinal output becomes all zeros

	return 0;
}

// required handshake with the sever
static int initClientNet(char* hostname, int port)
{
	struct sockaddr_in serv_addr;
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	struct hostent *server;
	if (sockfd < 0)
		error("ERROR opening socket");
	server = gethostbyname(hostname);
	if (server == NULL) {
		fprintf(stderr,"ERROR, no such host\n");
		exit(0);
	}
	bzero((char *) &serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	memcpy(&serv_addr.sin_addr.s_addr,server->h_addr,server->h_length);
	serv_addr.sin_port = htons(port);
	if (connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0)
		error("ERROR connecting");
	/* at this point, should be able to send/recv on sockfd */

	// connection successful with the client and server
	// since client goes first, call the init func
	log("initClientNet Line 154: Successfully connected to client");

	if (init("params") != 0) {
		log("initClientNet: Cannot init Diffie Hellman key exchange :(");
		printf("Cannot init Diffie Hellman key exchange :(");
		should_exit = true;
	}

	// generate user 1 key
	NEWZ(global_user1_sk);
	NEWZ(global_user1_pk);
	if(dhGen(global_user1_sk, global_user1_pk) != 0) {
		log("Something went wrong in dhGen() on the client, did you run init() function?");

		should_exit = true;
	}

	//store public key 1 (g^b mod p) to file "PublicKeyClient". 
	FILE *pk1 = fopen("PublicKeyClient", "w");
	mpz_out_str(pk1, base, global_user1_pk);
	fclose(pk1);

	// //read from file to get other persons public key 2.
	// FILE *pk2 = fopen("PublicKeyServer", "r");
	// mpz_inp_str(global_user2_pk, pk2, base);
	// fclose(pk2); 
	// error: dhfinal output becomes all zeros

	return 0;
}

static int shutdownNetwork()
{
	shutdown(sockfd,2);
	unsigned char dummy[64];
	ssize_t r;
	do {
		r = recv(sockfd,dummy,64,0);
	} while (r != 0 && r != -1);
	close(sockfd);
	return 0;
}

/* end network stuff. */


[[noreturn]] static void fail_exit(const char *msg)
{
	// Make sure endwin() is only called in visual mode. As a note, calling it
	// twice does not seem to be supported and messed with the cursor position.
	if (!isendwin())
		endwin();
	fprintf(stderr, "%s\n", msg);
	exit(EXIT_FAILURE);
}

// Checks errors for (most) ncurses functions. CHECK(fn, x, y, z) is a checked
// version of fn(x, y, z).
#define CHECK(fn, ...) \
	do \
	if (fn(__VA_ARGS__) == ERR) \
	fail_exit(#fn"("#__VA_ARGS__") failed"); \
	while (false)


// Message window
static WINDOW *msg_win;
// Separator line above the command (readline) window
static WINDOW *sep_win;
// Command (readline) window
static WINDOW *cmd_win;

// Input character for readline
static unsigned char input;

static int readline_getc(FILE *dummy)
{
	return input;
}

/* if batch is set, don't draw immediately to real screen (use wnoutrefresh
 * instead of wrefresh) */
static void msg_win_redisplay(bool batch, const string& newmsg="", const string& sender="")
{
	if (batch)
		wnoutrefresh(msg_win);
	else {
		wattron(msg_win,COLOR_PAIR(2));
		wprintw(msg_win,"%s:",sender.c_str());
		wattroff(msg_win,COLOR_PAIR(2));
		wprintw(msg_win," %s\n",newmsg.c_str());
		wrefresh(msg_win);
	}
}

static void msg_typed(char *line)
{
	if(isclient && !gotPK)
	{
		//read from file to get other persons public key 2.
		FILE *pk2 = fopen("PublicKeyServer", "r");
		mpz_inp_str(global_user2_pk, pk2, base);
		fclose(pk2);
		gotPK = true;
		//Get DH
		dhFinal(global_user1_sk,global_user1_pk,global_user2_pk,kA,klen);
		// for (size_t i = 0; i < klen; i++) {
		// 	printf("%02x ",kA[i]);
		// }

		//check if they got correct pk
		FILE *DH1 = fopen("DH1-PK2", "w");
		mpz_out_str(DH1, base, global_user2_pk);
		fclose(DH1);
		// verified: received correct pk
	}
	else if (!isclient && !gotPK)
	{
		//read from file to get other persons public key 1.
		FILE *pk1 = fopen("PublicKeyClient", "r");
		mpz_inp_str(global_user1_pk, pk1, base);
		fclose(pk1);
		gotPK = true;
		dhFinal(global_user2_sk,global_user2_pk,global_user1_pk,kB,klen);
		// for (size_t i = 0; i < klen; i++) {
		// 	printf("%02x ",kB[i]);
		// }

		//check if they got correct pk
		FILE *DH2 = fopen("DH2-PK1", "w");
		mpz_out_str(DH2, base, global_user1_pk);
		fclose(DH2); 
		//verified: received correct pk
	}

	if(isclient)
	{
		log("client's key:\n");
		for (size_t i = 0; i < klen; i++) {
			char text[10];
			sprintf(text, "%02x", kA[i]);
			log(text);
		}
	}

	else
	{
		log("\nserver's key:\n");
		for (size_t i = 0; i < klen; i++) {
			char text[10];
			sprintf(text, "%02x", kB[i]);
			log(text);
		}
		
	}


	string my_encrypted_msg;
	string line_str;
	if (!line) {
		// Ctrl-D pressed on empty line
		should_exit = true;
		/* XXX send a "goodbye" message so other end doesn't
		 * have to wait for timeout on recv()? */
	} else {
		if (*line) {
			// NOTE: please update the key params from "1234" to the other user's pk
			// waiting on that
			// if the client, then encrypt using the server's kA
			// if not client, then encrypt using the client's kB
			char* encrypted_line = isclient ? encryptMessage(line, "1234") : encryptMessage(line, "1234");

			add_history(encrypted_line);

			my_encrypted_msg = string(encrypted_line);
			line_str = string(line);
			
			transcript.push_back("me: " + line_str);

			ssize_t nbytes;
			if ((nbytes = send(sockfd,encrypted_line,my_encrypted_msg.length(),0)) == -1)
				error("send failed");
		}
		pthread_mutex_lock(&qmx);
		mq.push_back({false,line_str,"me",msg_win});
		pthread_cond_signal(&qcv);
		pthread_mutex_unlock(&qmx);
	}
}

/* if batch is set, don't draw immediately to real screen (use wnoutrefresh
 * instead of wrefresh) */
static void cmd_win_redisplay(bool batch)
{
	int prompt_width = strnlen(rl_display_prompt, 128);
	int cursor_col = prompt_width + strnlen(rl_line_buffer,rl_point);

	werase(cmd_win);
	mvwprintw(cmd_win, 0, 0, "%s%s", rl_display_prompt, rl_line_buffer);
	/* XXX deal with a longer message than the terminal window can show */
	if (cursor_col >= COLS) {
		// Hide the cursor if it lies outside the window. Otherwise it'll
		// appear on the very right.
		curs_set(0);
	} else {
		wmove(cmd_win,0,cursor_col);
		curs_set(1);
	}
	if (batch)
		wnoutrefresh(cmd_win);
	else
		wrefresh(cmd_win);
}

static void readline_redisplay(void)
{
	pthread_mutex_lock(&qmx);
	mq.push_back({false,"","",cmd_win});
	pthread_cond_signal(&qcv);
	pthread_mutex_unlock(&qmx);
}

static void resize(void)
{
	if (LINES >= 3) {
		wresize(msg_win,LINES-2,COLS);
		wresize(sep_win,1,COLS);
		wresize(cmd_win,1,COLS);
		/* now move bottom two to last lines: */
		mvwin(sep_win,LINES-2,0);
		mvwin(cmd_win,LINES-1,0);
	}

	/* Batch refreshes and commit them with doupdate() */
	msg_win_redisplay(true);
	wnoutrefresh(sep_win);
	cmd_win_redisplay(true);
	doupdate();
}

static void init_ncurses(void)
{
	if (!initscr())
		fail_exit("Failed to initialize ncurses");

	if (has_colors()) {
		CHECK(start_color);
		CHECK(use_default_colors);
	}
	CHECK(cbreak);
	CHECK(noecho);
	CHECK(nonl);
	CHECK(intrflush, NULL, FALSE);

	curs_set(1);

	if (LINES >= 3) {
		msg_win = newwin(LINES - 2, COLS, 0, 0);
		sep_win = newwin(1, COLS, LINES - 2, 0);
		cmd_win = newwin(1, COLS, LINES - 1, 0);
	} else {
		// Degenerate case. Give the windows the minimum workable size to
		// prevent errors from e.g. wmove().
		msg_win = newwin(1, COLS, 0, 0);
		sep_win = newwin(1, COLS, 0, 0);
		cmd_win = newwin(1, COLS, 0, 0);
	}
	if (!msg_win || !sep_win || !cmd_win)
		fail_exit("Failed to allocate windows");

	scrollok(msg_win,true);

	if (has_colors()) {
		// Use white-on-blue cells for the separator window...
		CHECK(init_pair, 1, COLOR_WHITE, COLOR_BLUE);
		CHECK(wbkgd, sep_win, COLOR_PAIR(1));
		/* NOTE: -1 is the default background color, which for me does
		 * not appear to be any of the normal colors curses defines. */
		CHECK(init_pair, 2, COLOR_MAGENTA, -1);
	}
	else {
		wbkgd(sep_win,A_STANDOUT); /* c.f. man curs_attr */
	}
	wrefresh(sep_win);
}

static void deinit_ncurses(void)
{
	delwin(msg_win);
	delwin(sep_win);
	delwin(cmd_win);
	endwin();
}

static void init_readline(void)
{
	// Let ncurses do all terminal and signal handling
	rl_catch_signals = 0;
	rl_catch_sigwinch = 0;
	rl_deprep_term_function = NULL;
	rl_prep_term_function = NULL;

	// Prevent readline from setting the LINES and COLUMNS environment
	// variables, which override dynamic size adjustments in ncurses. When
	// using the alternate readline interface (as we do here), LINES and
	// COLUMNS are not updated if the terminal is resized between two calls to
	// rl_callback_read_char() (which is almost always the case).
	rl_change_environment = 0;

	// Handle input by manually feeding characters to readline
	rl_getc_function = readline_getc;
	rl_redisplay_function = readline_redisplay;

	rl_callback_handler_install("> ", msg_typed);
}

static void deinit_readline(void)
{
	rl_callback_handler_remove();
}

static const char* usage =
"Usage: %s [OPTIONS]...\n"
"Secure chat for CSc380.\n\n"
"   -c, --connect HOST  Attempt a connection to HOST.\n"
"   -l, --listen        Listen for new connections.\n"
"   -p, --port    PORT  Listen or connect on PORT (defaults to 1337).\n"
"   -h, --help          show this message and exit.\n";

int main(int argc, char *argv[])
{
	// define long options
	static struct option long_opts[] = {
		{"connect",  required_argument, 0, 'c'},
		{"listen",   no_argument,       0, 'l'},
		{"port",     required_argument, 0, 'p'},
		{"help",     no_argument,       0, 'h'},
		{0,0,0,0}
	};
	// process options:
	char c;
	int opt_index = 0;
	int port = 1337;
	char hostname[HOST_NAME_MAX+1] = "localhost";
	hostname[HOST_NAME_MAX] = 0;
	// bool isclient = true;
	isclient = true;

	while ((c = getopt_long(argc, argv, "c:lp:h", long_opts, &opt_index)) != -1) {
		switch (c) {
			case 'c':
				if (strnlen(optarg,HOST_NAME_MAX))
					strncpy(hostname,optarg,HOST_NAME_MAX);
				break;
			case 'l':
				isclient = false;
				break;
			case 'p':
				port = atoi(optarg);
				break;
			case 'h':
				printf(usage,argv[0]);
				return 0;
			case '?':
				printf(usage,argv[0]);
				return 1;
		}
	}
	if (isclient) {
		initClientNet(hostname,port);
	} else {
		initServerNet(port);
	}

	/* NOTE: these don't work if called from cursesthread */
	init_ncurses();
	init_readline();
	/* start curses thread */
	if (pthread_create(&tcurses,0,cursesthread,0)) {
		fprintf(stderr, "Failed to create curses thread.\n");
	}
	/* start receiver thread: */
	if (pthread_create(&trecv,0,recvMsg,0)) {
		fprintf(stderr, "Failed to create update thread.\n");
	}

	/* put this in the queue to signal need for resize: */
	redraw_data rd = {false,"","",NULL};
	do {
		int c = wgetch(cmd_win);
		switch (c) {
			case KEY_RESIZE:
				pthread_mutex_lock(&qmx);
				mq.push_back(rd);
				pthread_cond_signal(&qcv);
				pthread_mutex_unlock(&qmx);
				break;
				// Ctrl-L -- redraw screen
			// case '\f':
			// 	// Makes the next refresh repaint the screen from scratch
			// 	/* XXX this needs to be done in the curses thread as well. */
			// 	clearok(curscr,true);
			// 	resize();
			// 	break;
			default:
				input = c;
				rl_callback_read_char();
		}
	} while (!should_exit);

	shutdownNetwork();
	deinit_ncurses();
	deinit_readline();
	return 0;
}

/* Let's have one thread responsible for all things curses.  It should
 * 1. Initialize the library
 * 2. Wait for messages (we'll need a mutex-protected queue)
 * 3. Restore terminal / end curses mode? */

/* We'll need yet another thread to listen for incoming messages and
 * post them to the queue. */

void* cursesthread(void* pData)
{
	/* NOTE: these calls only worked from the main thread... */
	// init_ncurses();
	// init_readline();
	while (true) {
		pthread_mutex_lock(&qmx);
		while (mq.empty()) {
			pthread_cond_wait(&qcv,&qmx);
			/* NOTE: pthread_cond_wait will release the mutex and block, then
			 * reaquire it before returning.  Given that only one thread (this
			 * one) consumes elements of the queue, we probably don't have to
			 * check in a loop like this, but in general this is the recommended
			 * way to do it.  See the man page for details. */
		}
		/* at this point, we have control of the queue, which is not empty,
		 * so write all the messages and then let go of the mutex. */
		while (!mq.empty()) {
			redraw_data m = mq.front();
			mq.pop_front();
			if (m.win == cmd_win) {
				cmd_win_redisplay(m.resize);
			} else if (m.resize) {
				resize();
			} else {
				msg_win_redisplay(false,m.msg,m.sender);
				/* Redraw input window to "focus" it (otherwise the cursor
				 * will appear in the transcript which is confusing). */
				cmd_win_redisplay(false);
			}
		}
		pthread_mutex_unlock(&qmx);
	}
	return 0;
}

void* recvMsg(void*)
{
	size_t maxlen = 256;
	char msg[maxlen+1];
	ssize_t nbytes;
	while (1) {
		if ((nbytes = recv(sockfd,msg,maxlen,0)) == -1)
			error("recv failed");
		msg[nbytes] = 0; /* make sure it is null-terminated */
		if (nbytes == 0) {
			/* signal to the main loop that we should quit: */
			should_exit = true;
			return 0;
		} 

		// decrypt the message here
		const char* decryptedMessage = decryptMessage(msg, "1234");
		pthread_mutex_lock(&qmx);
		mq.push_back({false,msg,"Mr Thread",msg_win});
		pthread_cond_signal(&qcv);
		pthread_mutex_unlock(&qmx);
	}
	return 0;
}
