//
// js2midi - convert any number of joystick event streams to a single midi event stream
//
//	This source is an expanded version of rbd2midi by
//	gigadude from http://www.hydrogen-music.org/hcms/node/1135
//	
//	The purpose being to make it support any number of drum sets and have 100%
//	configurable output events as well as support analog axes as well as buttons.
//	on first run, a .js2midirc will be created in your $HOME directory.
//	midi event numbers are listed at the top for convenience. The format should
//	be pretty self-explanatory. 
//	-----------------
//	[DEVICE]
//	a#=EVENT
//	b#=EVENT
//	-----------------
//	lines are ignored after the first # symbol. DEVICE is the joystick device in /dev
//	a#, b# there # is and integer that maps to axis or button number from the most
//	recently defined device to a midi instrument event number EVENT.
//	
//	
// building:
//
// > g++ -o js2midi js2midi.cpp -lasound -lpthread
//
// example usage:
//
// > # start up js2midi
// > js2midi &
// Opened "js2midi" [129:0]
//
// > # find out the available ports:
// > aconnect -i -o -l
//
// client 0: 'System' [type=kernel]
// 0 'Timer '
// 1 'Announce	 '
// Connecting To: 15:0
// client 14: 'Midi Through' [type=kernel]
// 0 'Midi Through Port-0'
// client 128: 'TiMidity' [type=user]
// 0 'TiMidity port 0 '
// 1 'TiMidity port 1 '
// 2 'TiMidity port 2 '
// 3 'TiMidity port 3 '
// client 129: 'js2midi' [type=user]
// 0 'js2midi'
// client 130: 'Hydrogen' [type=user]
// 0 'Hydrogen Midi-In'
//
// # connect to hydrogen (http://www.hydrogen-music.org/)
// > aconnect 129:0 130:0
//

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <linux/joystick.h>
#include <alsa/input.h>
#include <alsa/asoundlib.h>

#define js_devices 16
#define max_buttons 16
#define max_axes 12
// this is the name that other apps will see
static const char *my_name = "js2midi";

typedef struct str_thdata
{
    int thread_no;
} thdata;

// for more printy goodness
static const int verbose = 0;

static snd_seq_t *seq_handle = NULL;
static int my_client, my_port;
static snd_seq_event_t ev;
static int hhShift=0;
// by allowing the user to set these and doing a snd_seq_connect_to(...)
// we could drive a midi synth directly (instead of needing aconnect)
// for now just hard-wire us as a generic application input device
static const int seq_client = SND_SEQ_ADDRESS_SUBSCRIBERS;
static const int seq_port = 0;
static const int chan_no = 10;

int buttons[js_devices][max_buttons];
int axes[js_devices][max_axes];
char* devices[js_devices];
int fd[js_devices];
//
// close_sequencer - clean up and shut down
//

void close_sequencer()
{
	fprintf(stderr, "Closing \"%s\" [%d:%d]\n", my_name, my_client, my_port );
	if (seq_handle != NULL)
	{
		snd_seq_close( seq_handle );
		seq_handle = NULL;
	}
}

//
// open_sequencer - open the sequencer and advertise our midi input
//

int open_sequencer()
{
	if (snd_seq_open( &seq_handle, "hw", SND_SEQ_OPEN_OUTPUT, 0 ) < 0)
	{
		fprintf( stderr, "Failed to open sequencer\n" );
		return( 0 );
	}

	my_client = snd_seq_client_id( seq_handle );
	snd_seq_set_client_name( seq_handle, my_name );
	//snd_seq_set_client_group( seq_handle, "input" );

	my_port = snd_seq_create_simple_port( seq_handle, my_name,
			SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
			SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION );
	if (my_port < 0)
	{
		fprintf( stderr, "Failed to create port\n" );
		close_sequencer();
		return 0;
	}

	printf( "Opened \"%s\" [%d:%d]\n", my_name, my_client, my_port );

	return( 1 );
}

//
// flush_events - flush our event queue
//

static void flush_events()
{
	snd_seq_drain_output( seq_handle );
}

//
// send_event - queue an event to go out of our midi port
//

static void send_event()
{
	snd_seq_ev_set_direct( &ev );
	snd_seq_ev_set_source( &ev, my_port );
	snd_seq_ev_set_dest( &ev, seq_client, seq_port );

	snd_seq_event_output( seq_handle, &ev );
	// apparently you really need to flush each event?!?
	flush_events();
}

//
// hit_drum - generate a drum hit or release
//

static void hit_drum( int note, int is_hit, int vel )
{
	if (verbose) fprintf( stderr, "drum %d %s\n", note, is_hit ? "hit" : "off" );
	is_hit ? snd_seq_ev_set_noteon( &ev, chan_no, note, vel ) : snd_seq_ev_set_noteoff( &ev, chan_no, note, 0 );
	send_event();
}

//
// main - generate midi notes from joystick inputs
//



void printconfig(char* devices[js_devices], int b[js_devices][max_buttons], int a[js_devices][max_axes])
{
	fprintf(stderr, "---start------------------\n");
	for(int y=0;y<js_devices;y++)
	{	
		if(devices[y])
		{
			fprintf(stderr, "[%s]\n", devices[y]);
			for(int x=0;x<max_buttons;x++)
			{
				if(b[y][x])
					fprintf(stderr, "b%d=%d\n", x, b[y][x]);
			}
			for(int w=0;w<max_axes;w++)
			{
				if(a[y][w])
					fprintf(stderr, "a%d=%d\n", w, a[y][w]);
			}
		}
	}
	fprintf(stderr, "----end-------------------\n");
}

void writeconfig(char* fn, char* devices[js_devices], int b[js_devices][max_buttons], int a[js_devices][max_axes])
{
	//set defaults
	devices[0]=(char*)"/dev/input/js0";
	devices[1]=(char*)"/dev/input/js1";
	b[0][0]=59;
	b[0][1]=49;
	b[0][2]=38;
	b[0][3]=46;
	b[0][4]=44;
	a[0][4]=51;
	a[0][5]=54;

	b[1][0]=41;
	b[1][1]=48;
	b[1][2]=43;
	b[1][3]=45;
	b[1][4]=35;
	b[1][6]=55;
	b[1][7]=52;
	b[1][8]=53;
	b[1][11]=60;
	b[1][12]=61;
	b[1][13]=62;
	b[1][14]=63;

	FILE *outF=fopen(fn, "w");
	char *header=(char*)"#joystick to midi config file\n#35 Bass Drum 2\n#36 Bass Drum 1\n#37 Side Stick/Rimshot\n#38 Snare Drum 1\n#39 Hand Clap\n#40 Snare Drum 2\n#41 Low Tom 2\n#42 Closed Hi-hat\n#43 Low Tom 1\n#44 Pedal Hi-hat\n#45 Mid Tom 2\n#46 Open Hi-hat\n#47 Mid Tom 1\n#48 High Tom 2\n#49 Crash Cymbal 1\n#50 High Tom 1\n#51 Ride Cymbal 1\n#52 Chinese Cymbal\n#53 Ride Bell\n#54 Tambourine\n#55 Splash Cymbal\n#56 Cowbell\n#57 Crash Cymbal 2\n#58 Vibra Slap\n#59 Ride Cymbal 2\n#60 High Bongo\n#61 Low Bongo\n#62 Mute High Conga\n#63 Open High Conga\n#64 Low Conga\n#65 High Timbale\n#66 Low Timbale\n#67 High Agogô\n#68 Low Agogô\n#69 Cabasa\n#70 Maracas\n#71 Short Whistle\n#72 Long Whistle\n#73 Short Güiro\n#74 Long Güiro\n#75 Claves\n#76 High Wood Block\n#77 Low Wood Block\n#78 Mute Cuíca\n#79 Open Cuíca\n#80 Mute Triangle\n#81 Open Triangle\n\n#----------\n#Wii Rock Band:\n#b0:blue\n#b1:green\n#b2:red\n#b3:yellow\n#b4:orange\n#b8:-\n#b9:+\n#a4:dpad up/down\n#a5:dpad left/right\n\n#----------\n#Xbox Rock Band\n#b0:green\n#b1:red\n#b2:blue\n#b3:yellow#b4:orange\n#b6:select\n#b7:start\n#b8:xbox\n#b11:dpad left\n#b12:dpad right\n#b13:dpad up\n#b14:dpad down\n\n";
	fprintf(outF, "%s", header);
	for(int y=0;y<js_devices;y++)
	{	
		if(devices[y])
		{
			fprintf(outF, "[%s]\n", devices[y]);
			for(int x=0;x<max_buttons;x++)
			{
				if(b[y][x])
					fprintf(outF, "b%d=%d\n", x, b[y][x]);
			}
			for(int w=0;w<max_axes;w++)
			{
				if(a[y][w])
					fprintf(outF, "a%d=%d\n", w, a[y][w]);
			}
			fprintf(outF, "\n");
		}
	}
	if(outF)
		fclose(outF);
}

void readconfig(char* fn, char* devices[js_devices], int b[js_devices][max_buttons], int a[js_devices][max_axes])
{
	for(int y=0;y<js_devices;y++)
	{
		devices[y]=NULL;
	for(int x=0;x<max_buttons;x++)
		b[y][x]=0;
	for(int x=0;x<max_axes;x++)
		a[y][x]=0;
	}
	int dev=-1;
	FILE *configFile=fopen(fn, "ro");
	if(!configFile)
	{//write a new one
		fprintf(stderr, "no %s. Creating a new one.\n", fn);
		writeconfig(fn, devices, b, a);
	}
	else
	{
		fprintf(stderr, "found %s.\n", fn);
		char buffer[80];
		char buffer2[79];
		buffer[0]=0;
		int bufpos=0;
		char c;
		while(!feof(configFile))
		{
			fscanf(configFile, "%c", &c);
			if(c=='#')
			{
				bufpos=0;
				while(1)
				{
					fscanf(configFile, "%c", &c);
					if(feof(configFile))
					{
						c='\n';
						break;
					}
					if(c=='\n')
					{
						break;
					}
				}
			}
			if(c=='\n')	//end of line
			{
				bufpos=0;
				if(buffer[0]=='[')	//device
				{
					if(buffer[strlen(buffer)-1]==']')
					{
						dev++;
						devices[dev]=(char*)calloc(strlen(buffer)-1, sizeof(char));
						strcpy(devices[dev], buffer+1);
						devices[dev][strlen(devices[dev])-1]=0;
					}
				}
				if(buffer[0]=='b')	//button
				{
					strcpy(buffer2, buffer+1);
					char *vptr=strchr(buffer2, '=');
					if(vptr)
					{
						int idx=vptr-buffer2;
						vptr++;
						buffer2[idx]=0;
						int btn=atoi(buffer2);
						int val=atoi(vptr);
						b[dev][btn]=val;
					}
				}
				if(buffer[0]=='a')	//axis
				{
					strcpy(buffer2, buffer+1);
					char *vptr=strchr(buffer2, '=');
					if(vptr)
					{
						int idx=vptr-buffer2;
						vptr++;
						buffer2[idx]=0;
						int btn=atoi(buffer2);
						int val=atoi(vptr);
						a[dev][btn]=val;
					}	
				}
				buffer[0]=0;
			}
			else	//add do buffer
			{
				if(bufpos<79)
				{
					if(isalnum(c)||(c=='=')||(c=='/')||(c=='[')||(c==']'))
					{
						buffer[bufpos]=c;
						buffer[bufpos+1]=0;
						bufpos++;
					}
				}
			}
		}
//		printconfig(devices, b, a);
		fclose(configFile);
	}
}

void* jslistener(void* dd)
{
	thdata *dt=(thdata*)dd;
	int d=dt->thread_no;
	if((d<0)||(d>=js_devices))
	{
		fprintf(stderr, "Should not be trying to use device %d!\n", d);
		return NULL;
	}
	fprintf(stderr, "Opened %s\n", devices[d]);
	char vel=127;
	struct js_event e;
//printf("1 %d\n", d);
	while(read( fd[d], &e, sizeof(e) ) > 0)
	{
//printf("2 %d\n", d);
		const char *init = (e.type & JS_EVENT_INIT) ? "INIT:" : "";
//printf("3 %d\n", d);
		const char *type = ((e.type & ~JS_EVENT_INIT) == JS_EVENT_BUTTON) ? "BUTTON" : "AXIS";
//printf("4 %d\n", d);
		if (verbose) printf( "time %d val %d type %s%s num %d\n", e.time, e.value, init, type, e.number );
		if ((e.type & ~JS_EVENT_INIT) == JS_EVENT_BUTTON)
		{
//printf("5 %d\n", d);
			vel=127;
//printf("6 %d\n", d);
			if(buttons[d][e.number])
			{	
				int midicode=buttons[d][e.number];
				if(midicode==44)
				{
					hhShift=e.value;
					//printf("hh pedal\n");
				}
				if(hhShift)	//open/closed hihat
					if(midicode==46)
						midicode=42;
				hit_drum(midicode, e.value, vel);
			}
//printf("7 %d\n", d);
		}
		else
		{
//printf("8 %d\n", d);
			int v=0;
//printf("9 %d\n", d);
			if(e.value)
				v=1;
//printf("10 %d\n", d);
			vel=abs(e.value/256);
//printf("11 %d\n", d);
			if(axes[d][e.number])
				hit_drum(axes[d][e.number], v, vel);
//printf("12 %d\n", d);
		}
	}
//printf("13 %d\n", d);
	flush_events();		
//printf("14 %d\n", d);
}

int main( int argc, char *argv[] )
{
	pthread_t thd[js_devices];
	thdata dt[js_devices];

	char *home=getenv("HOME");
	char *confPath=(char*)calloc(strlen(home)+12, sizeof(char));
	sprintf(confPath, "%s/.js2midirc", home);
	readconfig(confPath, devices, buttons, axes);

	if (!open_sequencer()) return( 1 );
	snd_seq_ev_set_controller( &ev, 0, 0, 0 );
	send_event();
	snd_seq_ev_set_pgmchange( &ev, chan_no, 0 );
	send_event();
	snd_seq_ev_set_pitchbend( &ev, chan_no, 0 );
	send_event();
	flush_events();


	for(int d=0;d<js_devices;d++)
	{
		fd[d]=-1;
		dt[d].thread_no=d;
		if(devices[d])
		{
			fd[d] = open( devices[d], O_RDONLY );
			if (fd[d] < 0)
				fprintf(stderr, "Cannot open %s\n", devices[d]);
			else
				if(pthread_create(&thd[d], NULL, &jslistener, &dt[d]))
					fprintf(stderr, "Error creating listener thread for %s\n", devices[d]);
				else
					usleep(5000);
		}
	}
	usleep(10000);//make sure threads are started
	int ct=0;
	for(int x=0;x<js_devices;x++)
	{
		if (fd[x] >= 0) 
		{
			ct++;
			pthread_join(thd[x], NULL);
			close( fd[x] );
			fd[x] = 0;
		}
	}
	close_sequencer();
	if(ct)
		return 0;
	return 1;
}

