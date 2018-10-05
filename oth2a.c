#include "oth2a.h"

#include <string.h>
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "h2client.h"
#include "log.h"

#define VERSION_MAX_STRLEN	12

static bool update_firmware(char * new_version);
static void write_to_flash(const char *data, size_t len);

static const char * TAG = "oth2a";

extern const char * VERSION;

static const char * oth2a_base_url = CONFIG_OTH2A_BASE_URL;
static const char * oth2a_latest_file = "latest";
static const char * oth2a_bin_file_template  = CONFIG_OTH2A_FIRMWARE_PREAMBLE "_%s.bin";

static char latest_version[VERSION_MAX_STRLEN];

// update info (global)
static struct {
	SemaphoreHandle_t semaphore;
	esp_ota_handle_t handle;
	bool error;
	int binary_file_size;
} update_info;

int oth2a_initialize(void)
{
	log(INFO, TAG, "Software version %s", VERSION);

	update_info.semaphore = xSemaphoreCreateBinary();
	if(update_info.semaphore == NULL){
		log(ERROR, TAG, "Unable to create oth2a semaphore");
		return -1;
	}
	xSemaphoreGive(update_info.semaphore);

	return 0;
}

bool oth2a_handle()
{
	bool r = false;
	if(oth2a_new_sw_available(latest_version, sizeof(latest_version))){
		log(INFO, TAG, "New firmware available");
		if(update_firmware(latest_version)){
			r = true;
			log(INFO, TAG, "Firmware updated properly");
		}
	}
	return r;
}

/**
 * Return the software version
 * @return string to the sw version
 */
const char * oth2a_sw_version()
{
	return VERSION;
}

bool oth2a_new_sw_available(char * version_buffer, size_t buffer_size)
{
	if(strlen(oth2a_base_url) == 0){
		log(WARNING, TAG, "No Server url found, don't check for updates.");
		return false;
	}

	struct h2client_request r = h2client_request_initialize();
	char url[strlen(oth2a_base_url) + strlen(oth2a_latest_file) + 1 + 1];
	sprintf(url, "%s/%s", oth2a_base_url, oth2a_latest_file);
	r.url = url;

	r.responsebody.method = H2_HANDLEBODY_BUFFER;
	r.responsebody.buffer = version_buffer;
	r.responsebody.buffer_size = buffer_size - 1;

	if(h2client_do_request(&r) && r.status == 200){
		// http output data is copied to version_buffer
		// make it a proper version string (http adds extra \n's)
		int i;
		bool set = false;
		for(i = 0; i < r.responsebody.size && !set; i++){
			if(version_buffer[i] == '\n'){
				version_buffer[i] = '\0';
				set = true;
			}
		}
		if(!set){
			version_buffer[r.responsebody.size] = '\0';
		}

		return strcmp(version_buffer, VERSION) != 0;
	}

	return false;
}

static bool update_firmware(char * new_version)
{
	if(!xSemaphoreTake(update_info.semaphore, (TickType_t)0)){
		log(ERROR, TAG, "Unable to acquire firmware update semaphore");
		return false;
	}

	char bin_file[strlen(oth2a_bin_file_template) + strlen(new_version)];
	char bin_url[strlen(oth2a_base_url) + sizeof(bin_file)];

	struct h2client_request r = h2client_request_initialize();

	esp_err_t err;

	const esp_partition_t * configured = esp_ota_get_boot_partition();
	const esp_partition_t * running = esp_ota_get_running_partition();
	const esp_partition_t * update_partition = NULL;

	// create path
	if(sprintf(bin_file, oth2a_bin_file_template, new_version) < 0){
		log(ERROR, TAG, "Error constructing firmware binary file path");
		goto error;
	}
	if(sprintf(bin_url, "%s/%s", oth2a_base_url, bin_file) < 0){
		log(ERROR, TAG, "Error constructing firmware binary url");
		goto error;
	}
	log(INFO, TAG, "Firmware blob that will be downloaded: %s", bin_url);

	if(configured == NULL){
		log(ERROR, TAG, "Trying to update a non OTA partitioned system, aborting update...");
		goto error;
	}

	if(configured != running){
		log(WARNING, TAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
				configured->address, running->address);
		log(WARNING, TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
	}
	log(INFO, TAG, "Running partition type %d subtype %d (offset 0x%08x)",
			running->type, running->subtype, running->address);

	update_partition = esp_ota_get_next_update_partition(NULL);
	log(INFO, TAG, "Writing to partition subtype %d at offset 0x%x",
			update_partition->subtype, update_partition->address);

	if(update_partition == NULL){
		log(ERROR, TAG, "Error getting update partition, aborting update...");
		goto error;
	}

	err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_info.handle);
	if(err != ESP_OK){
		log(ERROR, TAG, "esp_ota_begin failed, error=%d", err);
		goto error;
	}
	log(INFO, TAG, "esp_ota_begin succeeded");

	// update bookkeeping
	update_info.error = false;
	update_info.binary_file_size = 0;

	// setup request
	r.url = bin_url;
	r.responsebody.method = H2_HANDLEBODY_CALLBACK;
	r.responsebody.callback = write_to_flash;
	r.timeout_ms = 300000; // 5 minute timeout for firmware upgrade

	// As we are update the flash streaming, we can't check for
	// the HTTP status. The flash function will check if a proper firmware
	// header is in the downloaded data. If not, flashing is aborted.
	if(!h2client_do_request(&r)){
		esp_ota_end(update_info.handle);
		goto error;
	}

	// cleanup update
	if(esp_ota_end(update_info.handle) != ESP_OK){
		log(ERROR, TAG, "esp_ota_end failed!");
		goto error;
	}

	if(!update_info.error){
		err = esp_ota_set_boot_partition(update_partition);
		if(err != ESP_OK){
			log(ERROR, TAG, "esp_ota_set_boot_partition failed! err=0x%x", err);
			goto error;
		}

		log(INFO, TAG, "Prepare to restart system!");
		esp_restart();
	}

	xSemaphoreGive(update_info.semaphore);
	return true;

error:
	xSemaphoreGive(update_info.semaphore);
	return false;
}

static void write_to_flash(const char *data, size_t len)
{
	esp_err_t err;

	log(DEBUG, TAG, "write_to_flash(%d)", len);

	if(!update_info.error){
		err = esp_ota_write(update_info.handle, (const void *)data, len);
		if(err != ESP_OK){
			log(ERROR, TAG, "Error: esp_ota_write failed! err=0x%x", err);
			update_info.error = true;
		}
		update_info.binary_file_size += len;

		log(DEBUG, TAG, "Have written image length %d", update_info.binary_file_size);
	}else{
		log(INFO, TAG, "Update error, discarding data");
	}
}
