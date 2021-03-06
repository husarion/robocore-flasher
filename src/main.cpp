#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <getopt.h>
#include <fstream>

#include "TRoboCOREHeader.h"
#include "timeutil.h"
#include "HardFlasher.h"
#include "utils.h"
#include "console.h"
#include "signal.h"
#include "myFTDI.h"

#ifdef EMBED_BOOTLOADERS
#include "bootloaders.h"
#endif

#include <vector>
using namespace std;

int doHelp = 0;
int doUnprotect = 0, doProtect = 0, doFlash = 0,
    doDump = 0, doDumpEEPROM = 0, doRegister = 0, doSetup = 0, doFlashBootloader = 0,
    doTest = 0, doSwitchEdison = 0, doSwitchSTM32 = 0, doSwitchESP = 0, doEraseEEPROM = 0, doFixPermissions;
int regType = -1;
int doConsole = 0;
int noSettingsCheck = 0;

#define BEGIN_CHECK_USAGE() int found = 0; do {
#define END_CHECK_USAGE() if (found != 1) { if (found > 1) warn1(); else warn2(); usage(argv); return 1; } } while (0);
#define CHECK_USAGE(x) if(x) { found++; }
#define CHECK_USAGE_NO_INC(x) if (x && found == 0) { found++; }

void decodeKey(const char* source, char* target);

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
	fprintf(stderr, "Flashing CORE2:\n");
	fprintf(stderr, "  %s [--speed speed] file.hex\n", argv[0]);
	fprintf(stderr, "\n");
	fprintf(stderr, "Serial terminal:\n");
	fprintf(stderr, "  %s --console [--speed speed]\n", argv[0]);
	fprintf(stderr, "\n");
	fprintf(stderr, "RoboCORE management:\n");
	fprintf(stderr, "  %s --switch-to-edison-only connects FTDI to Edison debug port and keeps STM32 in reset\n", argv[0]);
	fprintf(stderr, "  %s --switch-to-edison      connects FTDI to Edison debug port\n", argv[0]);
	fprintf(stderr, "  %s --switch-to-stm         connects FTDI to STM32\n", argv[0]);
	fprintf(stderr, "\n");
	fprintf(stderr, "Other commands:\n");
	fprintf(stderr, "  %s <other options>\n", argv[0]);
	fprintf(stderr, "\r\n");
	fprintf(stderr, "other options:\r\n");
	fprintf(stderr, "       --help, --usage  prints this message\n");
	fprintf(stderr, "       --test           tests connection to CORE2\n");
	fprintf(stderr, "       --setup          setup option bits\n");
	fprintf(stderr, "       --protect        protects bootloader against\n");
	fprintf(stderr, "                        unintended modifications\n");
	fprintf(stderr, "       --unprotect      unprotects bootloader against\n");
	fprintf(stderr, "                        unintended modifications\n");
	fprintf(stderr, "       --dump           dumps device info\n");
	fprintf(stderr, "       --dump-eeprom    dumps emulated EEPROM content\n");
	fprintf(stderr, "       --erase-eeprom   erases emulated EEPROM content\n");
	fprintf(stderr, "       --debug          show debug messages\n");
}

void callback(uint32_t cur, uint32_t total)
{
	int width = 30;
	int ratio = cur * width / total;
	int id = cur / 2000;

	if (cur == (uint32_t) - 1)
	{
		LOG_NICE("\rProgramming device... ");
		int toClear = width + 25;
		for (int i = 0; i < toClear; i++)
			LOG_NICE(" ");
		LOG_NICE("\rProgramming device... ");
		return;
	}

	static int lastID = -1;
	if (id == lastID)
		return;
	lastID = id;

	LOG_NICE("\rProgramming device... [");
	for (int i = 0; i < width; i++)
	{
		if (i <= ratio)
			LOG_NICE("=");
		else
			LOG_NICE(" ");
	}
	LOG_NICE("] %4d kB / %4d kB %3d%%", cur / 1024, total / 1024, cur * 100 / total);
	LOG_DEBUG("uploading %4d kB / %4d kB %3d%%", cur / 1024, total / 1024, cur * 100 / total);
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
	int res;
	int regSerial = -1;
	uint32_t regVer = 0xffffffff;
	int headerId = -1;
	char boardKey[16];
	bool hasKey = false;

	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGINT, sigHandler);

	static struct option long_options[] =
	{
		{ "test",       no_argument,       &doTest ,  1 },
		{ "flash",      no_argument,       &doFlash,  1 },
#ifdef EMBED_BOOTLOADERS
		{ "flash-bootloader", no_argument, &doFlashBootloader, 1 },
#endif
		{ "unprotect",  no_argument,       &doUnprotect, 1 },
		{ "protect",    no_argument,       &doProtect,   1 },
		{ "dump",       no_argument,       &doDump,      1 },
		{ "dump-eeprom",  no_argument,     &doDumpEEPROM, 1 },
		{ "erase-eeprom", no_argument,     &doEraseEEPROM, 1 },
		{ "setup",      no_argument,       &doSetup,     1 },
		{ "register",   no_argument,       &doRegister,  1 },

		{ "serial",     required_argument, 0,       'i' },
		{ "version",    required_argument, 0,       'v' },
		{ "variant",    required_argument, 0,       100 },
		{ "header-id",  required_argument, 0,       'H' },
		{ "board-key", required_argument, 0,      'k' },

		{ "speed",      required_argument, 0,       's' },

		{ "switch-to-edison-only", no_argument, &doSwitchEdison, 2 },
		{ "switch-to-edison", no_argument, &doSwitchEdison, 1 },
		{ "switch-to-stm32",  no_argument, &doSwitchSTM32,  1 },
		{ "switch-to-esp-flash",  no_argument, &doSwitchESP,  1 },

		{ "no-settings-check",  no_argument, &noSettingsCheck,  1 },

		{ "usage",      no_argument,       &doHelp,   1 },
		{ "help",       no_argument,       &doHelp,   1 },

		{ "console",    no_argument,       &doConsole, 1 },
		{ "debug",      no_argument,       &log_debug, 1 },

#ifdef __linux__
		{ "fix-permissions", no_argument, &doFixPermissions, 1 },
#endif

		{ 0, 0, 0, 0 }
	};

	for (;;)
	{
		int option_index = 0;
		int c = getopt_long(argc, argv, "hs:d:", long_options, &option_index);
		if (c == -1)
			break;

		switch (c)
		{
		case 'h':
			doHelp = 1;
			break;
		case 's':
			speed = atoi(optarg);
			if (speed == 0)
				speed = -1;
			break;
		case 'i':
			regSerial = atoi(optarg);
			if (regSerial < 0)
			{
				printf("inavlid serial\r\n");
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
				printf("invalid version (part count: %d)\r\n", (int)parts.size());
				exit(1);
			}
			if (a < 0 || b < 0 || c < 0 || d < 0)
			{
				printf("invalid version (bad numbers)\r\n");
				exit(1);
			}
			if (a + b + c + d == 0)
			{
				printf("invalid version (all zero)\r\n");
				exit(1);
			}

			makeVersion(regVer, a, b, c, d);
		}
		break;
		case 100:
			if (strcmp(optarg, "core2") == 0)
			{
				regType = 3;
			}
			else if (strcmp(optarg, "core2-mini") == 0)
			{
				regType = 4;
			}
			else
			{
				printf("invalid variant\r\n");
				exit(1);
			}
			break;
		case 'H':
			headerId = atoi(optarg);
			if (headerId < 0 || headerId > 4)
			{
				printf("invalid header id\r\n");
				exit(1);
			}
			break;
		case 'k':
			decodeKey(optarg, boardKey);
			hasKey = true;
			break;
		}
	}

	const char* filePath = 0;
	if (optind < argc)
		filePath = argv[optind];

	doFlash = !!filePath;
	if (doHelp)
	{
		usage(argv);
		return 1;
	}

	if (doFlash && doConsole && !filePath)
		doFlash = 0;

	BEGIN_CHECK_USAGE();
	CHECK_USAGE(doTest);
	CHECK_USAGE(!doProtect && !doUnprotect && doFlash);
	CHECK_USAGE((doProtect || doUnprotect) && doFlash);
	CHECK_USAGE(doProtect && !doFlash);
	CHECK_USAGE(doUnprotect && !doFlash);
	CHECK_USAGE(doDump);
	CHECK_USAGE(doDumpEEPROM);
	CHECK_USAGE(doEraseEEPROM);
	CHECK_USAGE(doRegister && regSerial != -1 && regVer != 0xffffffff && regType != -1 && headerId != -1 && hasKey);
	CHECK_USAGE(doSetup);
	CHECK_USAGE(doFlashBootloader);
	CHECK_USAGE(doSwitchEdison);
	CHECK_USAGE(doSwitchSTM32);
	CHECK_USAGE(doSwitchESP);
	CHECK_USAGE(doFixPermissions);
	CHECK_USAGE_NO_INC(doConsole);
	END_CHECK_USAGE();

	int openBootloader = doTest || doFlash || doProtect || doUnprotect ||
	                     doDump || doDumpEEPROM || doRegister || doSetup || doFlashBootloader ||
	                     doEraseEEPROM;

	if (openBootloader)
	{
		HardFlasher *flasher = new HardFlasher();
		int s = speed;
		if (s == -1)
			s = 460800;
		flasher->setBaudrate(s);
		flasher->setCallback(&callback);
		if (doFlash)
		{
			LOG_DEBUG("loading file...");
			res = flasher->load(filePath);
			if (res != 0)
			{
				LOG("unable to load hex file");
				return 1;
			}
		}

		res = flasher->init();
		if (res != 0)
		{
			LOG("unable to initialize flasher");
			return 1;
		}

		bool unprotectDone = false;
		while (true)
		{
			bool initBootloader = !(doSwitchSTM32 || doSwitchEdison);
			res = flasher->start(initBootloader);
			if (res == 0)
			{
				uint32_t startTime = TimeUtilGetSystemTimeMs();

				if (doUnprotect && !unprotectDone)
				{
					LOG_NICE("Unprotecting bootloader... ");
					LOG_DEBUG("unprotecting bootloader...");
					res = flasher->unprotect();
					unprotectDone = true;
					continue;
				}
				if (doDump)
				{
					LOG_NICE("Dumping info...\r\n");
					LOG_DEBUG("dumping info...");
					res = flasher->dump();
				}
				if (doDumpEEPROM)
				{
					LOG_NICE("Dumping info...\r\n");
					LOG_DEBUG("dumping info...");
					res = flasher->dumpEmulatedEEPROM();
				}
				if (doEraseEEPROM)
				{
					LOG_NICE("Erasing info...\r\n");
					LOG_DEBUG("Erasing info...");
					res = flasher->eraseEmulatedEEPROM();
				}
				if (doSetup)
				{
					LOG_NICE("Checking configuration... ");
					LOG_DEBUG("checking configuration...");
					res = flasher->setup();
				}
				if (doRegister)
				{
					HardFlasher *hf = (HardFlasher*)flasher;

					TRoboCOREHeader oldHeader;
					hf->readHeader(oldHeader, headerId);

					if (!oldHeader.isClear())
					{
						LOG_NICE("Already registered\r\n");
						LOG_DEBUG("already registered");
						break;
					}

					LOG_NICE("Registering...\r\n");
					LOG_DEBUG("registering...");
					TRoboCOREHeader h;
					h.headerVersion = 0x02;
					h.type = regType;
					h.version = regVer;
					h.id = regSerial;
					h.key[0] = 0x01;
					memcpy(h.key + 1, boardKey, 16);
					uint16_t crc = crc16_calc((uint8_t*)boardKey, 16);
					memcpy(h.key + 17, &crc, 2);
					res = hf->writeHeader(h);
				}
				if (doFlash)
				{
					LOG_NICE("Checking configuration... ");
					LOG_DEBUG("checking configuration...");
					res = flasher->setup(noSettingsCheck);
					if (res != 0)
					{
						continue;
					}

					LOG_NICE("Erasing device... ");
					LOG_DEBUG("erasing device...");
					res = flasher->erase();
					if (res != 0)
					{
						printf("\n");
						continue;
					}

					LOG_NICE("Programming device... ");
					LOG_DEBUG("programming device...");
					res = flasher->flash();
					if (res != 0)
					{
						printf("\n");
						continue;
					}

					if (!doProtect)
					{
						LOG_NICE("Reseting device... ");
						LOG_DEBUG("reseting device...");
						res = flasher->reset();
						if (res != 0)
						{
							printf("\n");
							continue;
						}
					}

					uint32_t endTime = TimeUtilGetSystemTimeMs();
					float time = endTime - startTime;
					float avg = flasher->getHexFile().totalLength / (time / 1000.0f) / 1024.0f;

					LOG_NICE("==== Summary ====\nTime: %d ms\nSpeed: %.2f KBps (%d bps)\n", endTime - startTime, avg, (int)(avg * 8.0f * 1024.0f));
				}
				if (doProtect)
				{
					LOG_NICE("Protecting bootloader... ");
					LOG_DEBUG("protecting bootloader...");
					res = flasher->protect();

					if (doFlash)
					{
						LOG_NICE("Reseting device... ");
						LOG_DEBUG("reseting device...");
						res = flasher->reset();
						if (res != 0)
						{
							printf("\n");
							continue;
						}
					}
				}
#ifdef EMBED_BOOTLOADERS
				if (doFlashBootloader)
				{
					static int stage = 0;

					if (stage == 0)
					{
						LOG_NICE("Checking version... ");
						LOG_DEBUG("checking version... ");
						TRoboCOREHeader h;
						HardFlasher *hf = (HardFlasher*)flasher;
						res = hf->readHeader(h);
						if (h.isClear())
						{
							LOG_NICE("unable, device is unregistered, register it first.\r\n");
							LOG_DEBUG("unable, device is unregistered, register it first.");
							break;
						}
						else if (!h.isValid())
						{
							LOG_NICE("header is invalid, unable to recognize device.\r\n");
							LOG_DEBUG("header is invalid, unable to recognize device.");
							break;
						}

						LOG_NICE("OK\r\n");
						LOG_DEBUG("OK");
						char buf[50];
						int a, b, c, d;
						parseVersion(h.version, a, b, c, d);
						switch (h.type)
						{
						// case 1: sprintf(buf, "bootloader_%d_%d_%d_mini", a, b, c); break;
						case 2: sprintf(buf, "bootloader_%d_%d_%d_big", a, b, c); break;
						case 3: sprintf(buf, "bootloader_%d_%d_%d_pro", a, b, c); break;
						default:
							LOG_NICE("Unsupported version\r\n");
							LOG_DEBUG("unsupported version");
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
							LOG_NICE("Bootloader not found\r\n");
							LOG_DEBUG("bootloader not found");
							break;
						}

						LOG_NICE("Bootloader found\r\n");
						LOG_DEBUG("bootloader found");

						flasher->loadData(ptr->data);

						LOG_NICE("Checking configuration... ");
						LOG_DEBUG("checking configuration...");
						res = flasher->setup();
						if (res != 0)
						{
							continue;
						}

						LOG_NICE("Unprotecting bootloader... ");
						LOG_DEBUG("unprotecting bootloader...");
						res = flasher->unprotect();
						if (res != 0)
						{
							continue;
						}

						stage = 1; // device must be reseted after unprotecting
						LOG_NICE("Resetting device\r\n");
						LOG_DEBUG("resetting device");
						continue;
					}
					else if (stage == 1)
					{
						LOG_NICE("Erasing device... ");
						LOG_DEBUG("erasing device...");
						res = flasher->erase();
						if (res != 0)
						{
							printf("\n");
							continue;
						}

						LOG_NICE("Programming device... ");
						LOG_DEBUG("programming device...");
						res = flasher->flash();
						if (res != 0)
						{
							printf("\n");
							continue;
						}

						LOG_NICE("Protecting bootloader... ");
						LOG_DEBUG("protecting bootloader...");
						res = flasher->protect();
						if (res != 0)
						{
							printf("\n");
							continue;
						}

						LOG_NICE("Reseting device... ");
						LOG_DEBUG("reseting device...");
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

		bool reset = !(doSwitchSTM32 || doSwitchEdison);
		flasher->cleanup(reset);
	}
	else if (doSwitchEdison)
	{
		LOG_NICE("Switching to Edison mode... ");
		LOG_DEBUG("switching to Edison mode...");
		int res = uart_switch_to_edison(doSwitchEdison == 2);
		if (res != 0)
		{
			printf("\n");
			return 1;
		}
	}
	else if (doSwitchSTM32)
	{
		LOG_NICE("Switching to STM32 mode... ");
		LOG_DEBUG("switching to STM32 mode...");
		int res = uart_switch_to_stm32();
		if (res != 0)
		{
			printf("\n");
			return 1;
		}
	}
	else if (doSwitchESP)
	{
		LOG_NICE("Switching to ESP flash mode... ");
		LOG_DEBUG("switching to ESP flash mode...");
		int res = uart_switch_to_esp();
		if (res != 0)
		{
			printf("\n");
			return 1;
		}
	}

	if (doConsole)
	{
		int s = speed;
		if (s == -1)
			s = 460800;
		return runConsole(s);
	}

#ifdef __linux__
	if (doFixPermissions) {
		if (getuid() != 0) {
			fprintf(stderr, "Run --fix-permissions as root\n");
			exit(1);
		}

		const char* user = getenv("SUDO_USER");
		if (user == nullptr || std::string(user) == "root") {
			fprintf(stderr, "Run --fix-permissions via sudo or set your normal username in SUDO_USER environment variable.\n");
			exit(1);
		}
		std::string rule = "SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"0403\", ATTRS{idProduct}==\"6015\", OWNER:=\"" + std::string(user) + "\", GROUP:=\"dialout\", MODE:=\"0664\"\n";
		std::string path = "/etc/udev/rules.d/90-core2-ftdi.rules";
		{
			std::ofstream f (path);
			f << rule;
		}
		system("udevadm control --reload-rules && udevadm trigger");

		fprintf(stderr, "Fixed permissions. You may now retry the operation you wanted to do.\n\n");
		exit(0);
	}
#endif

	return 0;
}

void decodeKey(const char* source, char* target)
{
	if (strlen(source) != 32)
	{
		printf("invalid key length (%s)\n", source);
		exit(1);
	}
	for (int i = 0; i < 16; i ++)
	{
		char byte[3];
		byte[0] = source[i * 2];
		byte[1] = source[i * 2 + 1];
		byte[2] = 0;
		int val;
		int read = sscanf(byte, "%x", &val);
		if (read != 1 || !isxdigit(byte[0]) || !isxdigit(byte[1]))
		{
			printf("invalid key");
			exit(1);
		}
		target[i] = (char)val;
	}
}
