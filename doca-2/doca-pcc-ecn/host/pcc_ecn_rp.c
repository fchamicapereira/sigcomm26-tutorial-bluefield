/*
 * Minimal DOCA PCC reaction-point host loader for the pure-ECN controller
 * (device/rp_main.c + device/algo/rtt_template.c).
 *
 * Trimmed from NVIDIA's DOCA PCC "pcc" sample host loader (pcc.c/pcc_core.c) down to
 * exactly what this tutorial needs: open one IB device by name, load our one compiled
 * RP algorithm image, run until Ctrl-C. No NP role, no switch-telemetry, no mailbox,
 * no IFA1/IFA2 probes, no per-parameter CLI overrides. Two flags: -d/--device
 * (this file) and -l/--log-level (built into doca_argp) -- see run.sh / the
 * top-level README's "Building and running DOCA PCC" section.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>

#include <doca_argp.h>
#include <doca_dev.h>
#include <doca_pcc.h>
#include <doca_error.h>

/* Compiled DPA image for our one algorithm (device/rp_main.c); produced by
 * build_device_code.sh and linked in as a static lib exposing this symbol
 * (name = --app-name passed to dpacc there). */
extern struct doca_pcc_app *pcc_ecn_rp_app;

#define LOG_LEVEL_ERROR (30)
#define LOG_LEVEL_INFO (50)
/* +1 thread is for inter-thread communication; this count is the DOCA PCC RP minimum. */
#define PCC_RP_THREADS_NUM (48 + 1)
#define PCC_COREDUMP_FILE_PATH "/tmp/doca_pcc_ecn_coredump.txt"
#define PCC_PRINT_BUFFER_SIZE (512 * 2048)

static int log_level = LOG_LEVEL_INFO;
#define PRINT_INFO(...)                                   \
  do {                                                    \
    if (log_level >= LOG_LEVEL_INFO) printf(__VA_ARGS__); \
  } while (0)
#define PRINT_ERROR(...)                                   \
  do {                                                     \
    if (log_level >= LOG_LEVEL_ERROR) printf(__VA_ARGS__); \
  } while (0)

/*
 * Fixed RP thread affinity, required by DOCA PCC RP. This is the DPA thread pool the
 * firmware schedules PCC work on -- taken as-is from NVIDIA's stock sample, not
 * something a controller author tunes.
 */
static const uint32_t rp_threads[PCC_RP_THREADS_NUM] = {
    176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 192, 193, 194, 195, 196,
    197, 198, 199, 200, 201, 202, 203, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217,
    218, 219, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 240};

/* Format string for the one doca_pcc_dev_trace_5() call in device/rp_main.c's init log. */
static char *trace_message_formats[] = {
    "format 0 - user init: port num = %#lx, algo index = %#lx, algo slot = %#lx, algo enable = "
    "%#lx, disable event bitmask = %#lx\n",
    NULL};

static volatile sig_atomic_t stop_requested;

static void sigint_handler(int dummy) {
  (void)dummy;
  stop_requested = 1;
  signal(SIGINT, SIG_DFL);
}

struct app_config {
  char device_name[DOCA_DEVINFO_IBDEV_NAME_SIZE];
};

static doca_error_t device_name_callback(void *param, void *config) {
  struct app_config *cfg = config;
  const char *name = param;

  if (strnlen(name, DOCA_DEVINFO_IBDEV_NAME_SIZE) >= DOCA_DEVINFO_IBDEV_NAME_SIZE) {
    PRINT_ERROR("Error: device name too long (max %d chars)\n", DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
    return DOCA_ERROR_INVALID_VALUE;
  }
  strncpy(cfg->device_name, name, DOCA_DEVINFO_IBDEV_NAME_SIZE - 1);
  return DOCA_SUCCESS;
}

static doca_error_t register_params(void) {
  struct doca_argp_param *device_param;
  doca_error_t result;

  result = doca_argp_param_create(&device_param);
  if (result != DOCA_SUCCESS) return result;
  doca_argp_param_set_short_name(device_param, "d");
  doca_argp_param_set_long_name(device_param, "device");
  doca_argp_param_set_arguments(device_param, "<IB device name>");
  doca_argp_param_set_description(device_param, "IB device name that supports PCC (mandatory).");
  doca_argp_param_set_callback(device_param, device_name_callback);
  doca_argp_param_set_type(device_param, DOCA_ARGP_TYPE_STRING);
  doca_argp_param_set_mandatory(device_param);
  return doca_argp_register_param(device_param);
}

/* Find and open the IB device with the given name that supports the PCC RP role. */
static doca_error_t open_pcc_device(const char *device_name, struct doca_dev **doca_device) {
  struct doca_devinfo **dev_list;
  uint32_t nb_devs = 0;
  doca_error_t result;
  char ibdev_name[DOCA_DEVINFO_IBDEV_NAME_SIZE] = {0};

  result = doca_devinfo_create_list(&dev_list, &nb_devs);
  if (result != DOCA_SUCCESS) {
    PRINT_ERROR("Error: failed to list DOCA devices: %s\n", doca_error_get_descr(result));
    return result;
  }

  *doca_device = NULL;
  for (uint32_t i = 0; i < nb_devs; i++) {
    if (doca_devinfo_get_is_pcc_supported(dev_list[i]) != DOCA_SUCCESS) continue;
    if (doca_devinfo_get_ibdev_name(dev_list[i], ibdev_name, sizeof(ibdev_name)) != DOCA_SUCCESS)
      continue;
    if (strncmp(device_name, ibdev_name, DOCA_DEVINFO_IBDEV_NAME_SIZE) != 0) continue;
    result = doca_dev_open(dev_list[i], doca_device);
    break;
  }
  doca_devinfo_destroy_list(dev_list);

  if (*doca_device == NULL) {
    PRINT_ERROR("Error: no PCC-capable device named '%s' found\n", device_name);
    return DOCA_ERROR_NOT_FOUND;
  }
  return result;
}

int main(int argc, char **argv) {
  struct app_config cfg = {0};
  struct doca_dev *dev = NULL;
  struct doca_pcc *pcc = NULL;
  doca_error_t result;
  uint32_t min_threads, max_threads;
  int exit_status = EXIT_FAILURE;
  bool started = false;

  if (signal(SIGINT, sigint_handler) == SIG_ERR) {
    fprintf(stderr, "Error: failed to install SIGINT handler\n");
    return EXIT_FAILURE;
  }

  result = doca_argp_init("doca_pcc_ecn_rp", &cfg);
  if (result != DOCA_SUCCESS) {
    fprintf(stderr, "Error: failed to init argp: %s\n", doca_error_get_descr(result));
    return EXIT_FAILURE;
  }
  result = register_params();
  if (result != DOCA_SUCCESS) {
    PRINT_ERROR("Error: failed to register params: %s\n", doca_error_get_descr(result));
    goto argp_cleanup;
  }
  result = doca_argp_start(argc, argv);
  if (result != DOCA_SUCCESS) {
    PRINT_ERROR("Error: failed to parse arguments: %s\n", doca_error_get_descr(result));
    goto argp_cleanup;
  }
  result = doca_argp_get_log_level(&log_level);
  if (result != DOCA_SUCCESS) log_level = LOG_LEVEL_INFO;

  result = open_pcc_device(cfg.device_name, &dev);
  if (result != DOCA_SUCCESS) goto argp_cleanup;

  result = doca_pcc_create(dev, &pcc);
  if (result != DOCA_SUCCESS) {
    PRINT_ERROR("Error: failed to create DOCA PCC context: %s\n", doca_error_get_descr(result));
    goto close_dev;
  }

  result = doca_pcc_get_min_num_threads(pcc, &min_threads);
  if (result == DOCA_SUCCESS) result = doca_pcc_get_max_num_threads(pcc, &max_threads);
  if (result != DOCA_SUCCESS || PCC_RP_THREADS_NUM < min_threads ||
      PCC_RP_THREADS_NUM > max_threads) {
    PRINT_ERROR("Error: RP thread count %d out of device-supported range\n", PCC_RP_THREADS_NUM);
    goto destroy_pcc;
  }

  result = doca_pcc_set_app(pcc, pcc_ecn_rp_app);
  if (result != DOCA_SUCCESS) {
    PRINT_ERROR("Error: failed to set DOCA PCC app: %s\n", doca_error_get_descr(result));
    goto destroy_pcc;
  }

  result = doca_pcc_set_thread_affinity(pcc, PCC_RP_THREADS_NUM, (uint32_t *)rp_threads);
  if (result != DOCA_SUCCESS) {
    PRINT_ERROR("Error: failed to set thread affinity: %s\n", doca_error_get_descr(result));
    goto destroy_pcc;
  }

  /* CCMAD is the plain RoCE CNP probe format (no IFA telemetry) -- what this tutorial uses. */
  result = doca_pcc_set_ccmad_probe_packet_format(pcc, 0);
  if (result == DOCA_SUCCESS) result = doca_pcc_rp_set_ccmad_remote_sw_handler(pcc, 0, false);
  if (result != DOCA_SUCCESS) {
    PRINT_ERROR("Error: failed to set CCMAD probe format: %s\n", doca_error_get_descr(result));
    goto destroy_pcc;
  }

  result = doca_pcc_set_print_buffer_size(pcc, PCC_PRINT_BUFFER_SIZE);
  if (result == DOCA_SUCCESS) result = doca_pcc_set_trace_message(pcc, trace_message_formats);
  if (result == DOCA_SUCCESS) result = doca_pcc_set_dev_coredump_file(pcc, PCC_COREDUMP_FILE_PATH);
  if (result != DOCA_SUCCESS) {
    PRINT_ERROR("Error: failed to configure DOCA PCC context: %s\n", doca_error_get_descr(result));
    goto destroy_pcc;
  }

  PRINT_INFO("Info: starting DOCA PCC pure-ECN RP controller on %s\n", cfg.device_name);
  result = doca_pcc_start(pcc);
  if (result != DOCA_SUCCESS) {
    PRINT_ERROR("Error: failed to start PCC: %s\n", doca_error_get_descr(result));
    goto destroy_pcc;
  }
  started = true;

  PRINT_INFO("Info: running -- Ctrl-C to stop\n");
  while (!stop_requested) {
    doca_pcc_process_state_t state;

    result = doca_pcc_get_process_state(pcc, &state);
    if (result != DOCA_SUCCESS) {
      PRINT_ERROR("Error: failed to query PCC state: %s\n", doca_error_get_descr(result));
      break;
    }
    if (state == DOCA_PCC_PS_DEACTIVATED || state == DOCA_PCC_PS_ERROR) {
      PRINT_ERROR("Error: PCC process state = %d, stopping\n", (int)state);
      break;
    }
    result = doca_pcc_wait(pcc, -1);
    if (result != DOCA_SUCCESS) {
      PRINT_ERROR("Error: doca_pcc_wait failed: %s\n", doca_error_get_descr(result));
      break;
    }
  }

  exit_status = EXIT_SUCCESS;

destroy_pcc:
  if (started) doca_pcc_stop(pcc);
  doca_pcc_destroy(pcc);
close_dev:
  doca_dev_close(dev);
argp_cleanup:
  doca_argp_destroy();
  return exit_status;
}
