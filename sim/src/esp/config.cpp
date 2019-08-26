#include "config.h"
#include <fstream>

config::ConfigManager config::manager;
const char * config::entry_names[] = {
	"ssid",
	"psk",
	"ntpserver",

	// TTC PARAMETERS
	"stopid1",
	"stopid2",
	"stopid3",
	"dirtag1",
	"dirtag2",
	"dirtag3",
	"shortname1",
	"shortname2",
	"shortname3",

	// WEATHER PARAMETERS
	"wapikey",
	"wapilat",
	"wapilon",

	// CONFIG PARAMETERS
	"configuser",
	"configpass",
	
	// SCCFG PARAMETERS
	"scttc",
	"scweather",
	"sc3d",
	"sctimes",
	"sccalfix"
};


config::ConfigManager::ConfigManager() {
	this->data = (char *)malloc(128);
	memset(this->offsets, 0xFF, sizeof(this->offsets));
}

bool config::ConfigManager::use_new_config(const char * data, uint32_t size) {
	{
		std::ofstream cfg("config.txt", std::ios::trunc);
		cfg.write(data, size);
		cfg.close();
	}

	free(this->data);
	this->data = (char *)malloc(128);
	this->size = 128;
	this->ptr = 0;
	memset(this->offsets, 0xFF, sizeof(this->offsets));

	load_from_sd();

	return true;
}

const char * config::ConfigManager::get_value(config::Entry e, const char * value) {
	if (this->offsets[e] == 0xFFFF) {
		return value;
	}
	return (this->data + this->offsets[e]);
}

void config::ConfigManager::load_from_sd() {
	// Make sure the config file exists before we read it.
	
	std::ifstream config("config.txt");
	if (!config.good()) {
		Serial1.println(F("Config file missing, seed it with ssid/psdk/url and make the last entry update=now, or place the entire config there"));
		delay(1000);
		ESP.restart();
	}

	// Begin parsing it.
	char entry_name[16];
	char entry_value[256];
	bool mode = false;
	uint8_t pos = 0;
	
	while (!config.eof()) {
		char c = config.get();
		if (mode) {
			if (c != '\n') {
				entry_value[pos++] = c;
			}
			else {
				entry_value[pos++] = 0;

				int e;
				for (e = 0; e < config::ENTRY_COUNT; ++e) {
					if (strcmp(config::entry_names[e], entry_name) == 0) {
						add_entry(static_cast<Entry>(e), entry_value);
						Serial1.printf("Set %s (%02x) = %s\n", entry_name, e, entry_value);
						break;
					}
				}

				if (e == config::ENTRY_COUNT) Serial1.printf("Invalid key %s\n", entry_name);

				mode = false;
				pos = 0;
			}
		}
		else {
			if (c == '\n') {
				pos = 0;
			}
			else if (c == '=') {
				entry_name[pos++] = 0;
				pos = 0;
				mode = true;
			}
			else {
				entry_name[pos++] = c;
			}
		}
	}

	config.close();
}

void config::ConfigManager::add_entry(Entry e, const char * value) {
	uint8_t length = strlen(value) + 1;
	if (this->ptr + length >= this->size) {
		this->size += length * 2;
		this->data = (char *)realloc(this->data, this->size);
	}

	this->offsets[e] = this->ptr;
	memcpy(this->data + this->ptr, value, length);
	this->ptr += length;
}