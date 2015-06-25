#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <getopt.h>

#include "TRoboCOREHeader.h"
#include "timeutil.h"
#include "Flasher.h"
#include "HardFlasher.h"
#include "SoftFlasher.h"
#include "utils.h"
#include "console.h"
#include "signal.h"

#ifdef EMBED_BOOTLOADERS
#include "bootloaders.h"
#endif

#include <vector>
using namespace std;

int doHelp = 0;
int doSoft = 0, doHard = 0, doUnprotect = 0, doProtect = 0, doFlash = 0,
    doDump = 0, doRegister = 0, doSetup = 0, doFlashBootloader = 0, doTest = 0;
int regType = -1;
int doConsole = 0;

#define BEGIN_CHECK_USAGE() int found = 0; do {
#define END_CHECK_USAGE() if (found != 1) { if (found > 1) warn1(); else warn2(); usage(argv); return 1; } } while (0);
#define CHECK_USAGE(x) if(x) { found++; }
#define CHECK_USAGE_NO_INC(x) if(x && found == 0) { found++; }

uint32_t getTicks()
{
	timeval tv;
	gettimeofday(&tv, 0);
	uint32_t val = tv.tv_sec * 1000 + tv.tv_usec / 1000;
	return val;
}

void warn1()
{
	printf("only one action is allowed\r\n\r\n");
}
void warn2()
{
	printf("invalid action\r\n\r\n");
}
void usage(char** argv)
{
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Flashing RoboCORE:\n");
	fprintf(stderr, "  %s [--hard] [--speed speed] file.hex\n", argv[0]);
	fprintf(stderr, "  %s --soft --device /dev/ttyUSB0 file.hex\n", argv[0]);
	fprintf(stderr, "\n");
	fprintf(stderr, "Flashing bootloader:\n");
	fprintf(stderr, "  %s --flash-bootloader\n", argv[0]);
	fprintf(stderr, "\n");
	fprintf(stderr, "Other commands:\n");
	fprintf(stderr, "  %s <other options>\n", argv[0]);
	fprintf(stderr, "\r\n");
	fprintf(stderr, "other options:\r\n");
	fprintf(stderr, "       --help, --usage  prints this message\n");
	fprintf(stderr, "       --test           tests connection to RoboCORE\n");
	fprintf(stderr, "       --setup          setup option bits\n");
	fprintf(stderr, "       --protect        protects bootloader against\n");
	fprintf(stderr, "                        unintended modifications (only valid with --hard)\n");
	fprintf(stderr, "       --unprotect      unprotects bootloader against\n");
	fprintf(stderr, "                        unintended modifications (only valid with --hard)\n");
	fprintf(stderr, "       --dump           dumps device info (only valid with --hard)\n");
}

void callback(uint32_t cur, uint32_t total)
{
	int width = 30;
	int ratio = cur * width / total;
	int id = cur / 2000;

	if (cur == (uint32_t) - 1)
	{
		printf("\rProgramming device... ");
		int toClear = width + 25;
		for (int i = 0; i < toClear; i++)
			putchar(' ');
		printf("\rProgramming device... ");
		return;
	}

	static int lastID = -1;
	if (id == lastID)
		return;
	lastID = id;

	printf("\rProgramming device... [");
	for (int i = 0; i < width; i++)
	{
		if (i <= ratio)
			putchar('=');
		else
			putchar(' ');
	}
	printf("] %4d kB / %4d kB %3d%%", cur / 1024, total / 1024, cur * 100 / total);
	fflush(stdout);
}

static void sigHandler(int)
{
	uart_close();
	exit(0);
}

int main(int argc, char** argv)
{
	int speed = -1;
	const char* device = 0;
	int res;
	int regId = -1;
	uint32_t regVer = 0xffffffff;

	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGINT, sigHandler);

	static struct option long_options[] =
	{
		{ "soft",       no_argument,       &doSoft,   1 },
		{ "hard",       no_argument,       &doHard,   1 },

		{ "test",       no_argument,       &doTest ,  1 },
		{ "flash",      no_argument,       &doFlash,  1 },
#ifdef EMBED_BOOTLOADERS
		{ "flash-bootloader", no_argument, &doFlashBootloader, 1 },
#endif
		{ "unprotect",  no_argument,       &doUnprotect, 1 },
		{ "protect",    no_argument,       &doProtect,   1 },
		{ "dump",       no_argument,       &doDump,      1 },
		{ "setup",      no_argument,       &doSetup,     1 },
		{ "register",   no_argument,       &doRegister,  1 },

		{ "id",         required_argument, 0,       'i' },
		{ "version",    required_argument, 0,       'v' },
		{ "big",        no_argument,       &regType,  2 },
		{ "pro",        no_argument,       &regType,  3 },

		{ "device",     required_argument, 0,       'd' },
		{ "speed",      required_argument, 0,       's' },

		{ "usage",      no_argument,       &doHelp,   1 },
		{ "help",       no_argument,       &doHelp,   1 },

		{ "console",    no_argument,       &doConsole, 1 },

		{ 0, 0, 0, 0 }
	};

	for (;;)
	{
		int option_index = 0;
		char *p;
		int c = getopt_long(argc, argv, "hs:d:", long_options, &option_index);
		if (c == -1)
			break;

		switch (c)
		{
		case 'h':
			doHelp = 1;
			break;
		case 'd':
			device = optarg;
			break;
		case 's':
			speed = atoi(optarg);
			if (speed == 0)
				speed = -1;
			break;
		case 'i':
			regId = atoi(optarg);
			if (regId < 0)
			{
				printf("inavlid id\r\n");
				exit(1);
			}
			break;
		case 'v':
		{
			vector<string> parts = splitString(optarg, ".");
			int a, b, c, d;
			if (parts.size() == 3)
			{
				a = atoi(parts[0].c_str());
				b = atoi(parts[1].c_str());
				c = atoi(parts[2].c_str());
				d = 0;
			}
			else if (parts.size() == 4)
			{
				a = atoi(parts[0].c_str());
				b = atoi(parts[1].c_str());
				c = atoi(parts[2].c_str());
				d = atoi(parts[3].c_str());
			}
			else
			{
				printf("invalid version\r\n");
				exit(1);
			}
			if (a < 0 || b < 0 || c < 0 || d < 0)
			{
				printf("invalid version\r\n");
				exit(1);
			}
			if (a + b + c + d == 0)
			{
				printf("invalid version\r\n");
				exit(1);
			}

			makeVersion(regVer, a, b, c, d);
		}
		break;
		}
	}

	doHard = !doSoft;
	doFlash = !doProtect && !doUnprotect && !doDump && !doRegister &&
	          !doSetup && !doFlashBootloader && !doTest;
	if (doHelp)
	{
		usage(argv);
		return 1;
	}
	if (doFlash + doProtect + doUnprotect + doDump + doRegister + doSetup + doFlashBootloader + doTest > 1)
	{
		warn1();
		usage(argv);
		return 1;
	}

	const char* filePath = 0;
	if (optind < argc)
		filePath = argv[optind];

	if (doFlash && doConsole && !filePath)
		doFlash = 0;

	BEGIN_CHECK_USAGE();
	CHECK_USAGE(doHard && doTest);
	CHECK_USAGE(doHard && doFlash && filePath);
	CHECK_USAGE(doHard && doProtect);
	CHECK_USAGE(doHard && doUnprotect);
	CHECK_USAGE(doHard && doDump);
	CHECK_USAGE(doHard && doRegister && regId != -1 && regVer != 0xffffffff && regType != -1);
	CHECK_USAGE(doHard && doSetup);
	CHECK_USAGE(doHard && doFlashBootloader);
	CHECK_USAGE(doSoft && doFlash && device && filePath);
	CHECK_USAGE_NO_INC(doConsole);
	END_CHECK_USAGE();

	int openBootloader = doTest || doFlash || doProtect || doUnprotect || doDump || doRegister || doSetup ||
	                     doFlashBootloader;

	if (openBootloader)
	{
		Flasher *flasher = 0;
		if (doHard)
		{
			flasher = new HardFlasher();
			int s = speed;
			if (s == -1)
				s = 460800;
			flasher->setBaudrate(s);
		}
		if (doSoft)
		{
			flasher = new SoftFlasher();
			flasher->setBaudrate(460800);
			flasher->setDevice(device);
		}
		flasher->setCallback(&callback);
		if (doFlash)
		{
			res = flasher->load(filePath);
			if (res != 0)
			{
				printf("unable to load hex file\n");
				return 1;
			}
		}

		res = flasher->init();
		if (res != 0)
		{
			printf("unable to initialize flasher\n");
			return 1;
		}

		while (true)
		{
			res = flasher->start();
			if (res == 0)
			{
				uint32_t startTime = TimeUtilGetSystemTimeMs();

				if (doProtect)
				{
					printf("Protecting bootloader... ");
					res = flasher->protect();
				}
				else if (doUnprotect)
				{
					printf("Unprotecting bootloader... ");
					res = flasher->unprotect();
				}
				else if (doDump)
				{
					printf("Dumping info...\r\n");
					res = flasher->dump();
				}
				else if (doSetup)
				{
					printf("Checking configuration... ");
					res = flasher->setup();
				}
				else if (doRegister)
				{
					HardFlasher *hf = (HardFlasher*)flasher;

					TRoboCOREHeader oldHeader;
					hf->readHeader(oldHeader);

					if (!oldHeader.isClear())
					{
						printf("Already registered\r\n");
						break;
					}

					printf("Registering...\r\n");
					TRoboCOREHeader h;
					h.headerVersion = 0x02;
					h.type = regType;
					h.version = regVer;
					h.id = regId;
					h.key[0] = 0x00;
					res = hf->writeHeader(h);
				}
				else if (doFlash)
				{
					if (doHard)
					{
						printf("Checking configuration... ");
						res = flasher->setup();
						if (res != 0)
						{
							continue;
						}
					}

					printf("Erasing device... ");
					res = flasher->erase();
					if (res != 0)
					{
						printf("\n");
						continue;
					}

					printf("Programming device... ");
					res = flasher->flash();
					if (res != 0)
					{
						printf("\n");
						continue;
					}

					printf("Reseting device... ");
					res = flasher->reset();
					if (res != 0)
					{
						printf("\n");
						continue;
					}

					uint32_t endTime = TimeUtilGetSystemTimeMs();
					float time = endTime - startTime;
					float avg = flasher->getHexFile().totalLength / (time / 1000.0f) / 1024.0f;

					printf("==== Summary ====\nTime: %d ms\nSpeed: %.2f KBps (%d bps)\n", endTime - startTime, avg, (int)(avg * 8.0f * 1024.0f));
				}
#ifdef EMBED_BOOTLOADERS
				else if (doFlashBootloader)
				{
					static int stage = 0;

					if (stage == 0)
					{
						printf("Checking version... ");
						TRoboCOREHeader h;
						HardFlasher *hf = (HardFlasher*)flasher;
						res = hf->readHeader(h);
						if (h.isClear())
						{
							printf("unable, device is unregistered, register it first.\r\n");
							break;
						}
						else if (!h.isValid())
						{
							printf("header is invalid, unable to recognize device.\r\n");
							break;
						}

						printf("OK\r\n");
						char buf[50];
						int a, b, c, d;
						parseVersion(h.version, a, b, c, d);
						switch (h.type)
						{
						case 1: sprintf(buf, "bootloader_%d_%d_%d_mini", a, b, c); break;
						case 2: sprintf(buf, "bootloader_%d_%d_%d_big", a, b, c); break;
						case 3: sprintf(buf, "bootloader_%d_%d_%d_pro", a, b, c); break;
						default:
							printf("Unsupported version\r\n");
							break;
						}

						bool found = false;
						TBootloaderData* ptr = bootloaders;
						while (ptr->name)
						{
							if (strcmp(ptr->name, buf) == 0)
							{
								found = true;
								break;
							}
							ptr++;
						}

						if (!found)
						{
							printf("Bootloader not found\r\n");
							break;
						}

						printf("Bootloader found\r\n");

						flasher->loadData(ptr->data);

						printf("Checking configuration... ");
						res = flasher->setup();
						if (res != 0)
						{
							continue;
						}

						printf("Unprotecting bootloader... ");
						res = flasher->unprotect();
						if (res != 0)
						{
							continue;
						}

						stage = 1; // device must be reseted after unprotecting
						printf("Resetting device\r\n");
						continue;
					}
					else if (stage == 1)
					{
						printf("Erasing device... ");
						res = flasher->erase();
						if (res != 0)
						{
							printf("\n");
							continue;
						}

						printf("Programming device... ");
						res = flasher->flash();
						if (res != 0)
						{
							printf("\n");
							continue;
						}

						printf("Protecting bootloader... ");
						res = flasher->protect();
						if (res != 0)
						{
							printf("\n");
							continue;
						}

						printf("Reseting device... ");
						res = flasher->reset();
						if (res != 0)
						{
							printf("\n");
							continue;
						}

						// printf("searching for bootloader %s", buf);
						// printf("%s\n", buf);
					}
				}
#endif
				break;
			}
			else if (res == -2)
			{
				// printf("\r\n");
				break;
			}
		}

		flasher->cleanup();
	}

	if (doConsole)
	{
		int s = speed;
		if (s == -1)
			s = 230400;
		return runConsole(s);
	}

	return 0;
}
