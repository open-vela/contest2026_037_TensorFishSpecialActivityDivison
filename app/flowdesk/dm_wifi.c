/****************************************************************************
 * dm_wifi.c — WiFi backend aligned with official Gemini-S1 / NuttX guide
 *
 * Official connect sequence (Gemini-S1 docs + 9.5 wifi_manager.c):
 *   ifup wlan0
 *   wapi mode wlan0 2          (WAPI_MODE_MANAGED / STA)
 *   wapi psk wlan0 <pass> 3 2  (3 = WPA_ALG_CCMP, 2 = WPA2)
 *   wapi essid wlan0 <ssid> 1  (1 = WAPI_ESSID_ON)
 *   renew wlan0
 *
 * Fixes applied:
 *   1. Do not write s_cur_ssid when DHCP fails (!got_ip); dm_wifi_connected()
 *      checks both s_cur_ssid and s_cur_ip so failed auto-connect never fakes
 *      "Connected" state.
 *   2. Manual scan/connect sets s_manual_override = 1, immediately interrupting
 *      background boot auto-connect so clicking "Scan" is never blocked by
 *      s_conn_busy.
 *   3. Robust wapi scan parser: skips non-MAC header/log lines, preserves SSIDs
 *      containing spaces, deduplicates BSSIDs, and sorts by RSSI descending.
 *   4. Avoids double DHCP calls in the same loop iteration.
 *   5. Credentials are saved only AFTER an IP was obtained and the requested
 *      ESSID verified (a wrong password no longer poisons boot auto-connect).
 *   6. Previous association/key is dropped before each attempt, and open APs
 *      explicitly clear any leftover WPA key (alg NONE).
 *   7. wapi command return codes are checked (psk/essid); FlowDesk saves
 *      verified credentials to its own /data files, avoiding a second
 *      WAPI/NSH startup owner.
 *   8. Hardcoded fallback SSID/password removed from source.
 *   9. Boot and link-loss auto-connect use only verified credentials from
 *      FlowDesk /data files or the existing WAPI JSON config; no SSID/PSK is
 *      compiled into the application.
 *  10. Ported from 9.5 wifi_manager.c: the connect sequence retries up to
 *      DM_WIFI_CONN_ROUNDS with a 5s gap, and uses "wapi show" (AP all-zero)
 *      as an extra association check in addition to the IPv4 check.
 ****************************************************************************/

#include "deskmate.h"

#ifdef CONFIG_DESKMATE_APP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <wireless/wapi.h>
#include <netutils/netlib.h>
#include <netutils/ntpclient.h>
#include <netutils/cJSON.h>
#include <net/if.h>
#include <syslog.h>

#define DM_WIFI_IF        "wlan0"
#define DM_WIFI_AP_MAX    24
#define DM_WIFI_STACK     65536
#define DM_WIFI_CONN_ROUNDS    4   /* 9.5 wifi_manager retries forever; 4x5s is enough */
#define DM_WIFI_CONN_RETRY_SEC 5
#define DM_WIFI_NTP_SERVERS \
  "ntp.aliyun.com;time2.cloud.tencent.com;time3.cloud.tencent.com"
#define DM_WIFI_NTP_RETRY_SEC 120
#define DM_WIFI_MANUAL_TIME_PATH "/data/deskmate_manual_time.txt"

/* Fallback AP when no saved credentials exist.
 * Keep empty so personal WiFi credentials are NOT hardcoded in the repository.
 * (Users can connect once from Settings > Wi-Fi; credentials are then saved
 *  to /data and reused for boot auto-connect.) */
#define DM_WIFI_AUTO_SSID ""
#define DM_WIFI_AUTO_PSK  ""
#define DM_WIFI_SAVE_PATH "/data/deskmate_wifi.txt"
#define DM_WIFI_WAPI_CONFIG_PATH "/data/etc/wifi/wapi.conf"

static volatile int s_drv_ready;
static volatile int s_auto_started;
static volatile int s_manual_override;
static volatile int s_conn_is_auto;
static volatile int s_reconnect_running;
static volatile int s_auto_thread_active;
static uint64_t s_reconnect_retry_after_ms;
static uint64_t s_ntp_retry_after_ms;
static int s_ntp_refresh_running;
static int s_ntp_servers_selected;
static volatile int s_manual_time_set;
static int s_clock_seed_only;
static pthread_mutex_t s_ntp_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t wifi_monotonic_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return 0;
    }
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void wifi_request_reconnect(void);

typedef struct
{
  char ssid[33];
  int rssi;
  bool open;
} dm_ap_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int s_scan_busy;
static volatile int s_conn_busy;
static volatile int s_scan_ready;
static volatile int s_conn_ok;
static volatile int s_conn_fail;
static volatile int s_wifi_enabled = 1;
static int s_poll_ticks;
static volatile int s_link_lost;
static dm_ap_t s_aps[DM_WIFI_AP_MAX];
static int s_ap_n;
static int s_sel = -1;
static char s_cur_ssid[33];
static char s_cur_ip[24];
static char s_pending_ssid[33];
static char s_pending_pass[64];
static bool s_pending_open;

const char *dm_wifi_cur_ssid(void)
{
  return s_cur_ssid[0] ? s_cur_ssid : "";
}

const char *dm_wifi_cur_ip(void)
{
  return s_cur_ip[0] ? s_cur_ip : "";
}

int dm_wifi_connected(void)
{
  return (s_cur_ssid[0] != 0 && s_cur_ip[0] != 0) ? 1 : 0;
}

int dm_wifi_ap_count(void)
{
  return s_ap_n;
}

int dm_wifi_ap_get(int i, const char **ssid, int *rssi, bool *open)
{
  if (i < 0 || i >= s_ap_n)
    {
      return -1;
    }
  if (ssid)
    {
      *ssid = s_aps[i].ssid;
    }
  if (rssi)
    {
      *rssi = s_aps[i].rssi;
    }
  if (open)
    {
      *open = s_aps[i].open;
    }
  return 0;
}

int dm_wifi_sel(void)
{
  return s_sel;
}

void dm_wifi_set_sel(int i)
{
  s_sel = i;
}

int dm_wifi_scan_busy(void)
{
  return s_scan_busy;
}

int dm_wifi_scan_ready(void)
{
  return s_scan_ready;
}

void dm_wifi_clear_scan_ready(void)
{
  s_scan_ready = 0;
}

int dm_wifi_conn_busy(void)
{
  return s_conn_busy;
}

int dm_wifi_conn_ok(void)
{
  return s_conn_ok;
}

int dm_wifi_conn_fail(void)
{
  return s_conn_fail;
}

void dm_wifi_clear_conn_flags(void)
{
  s_conn_ok = 0;
  s_conn_fail = 0;
}

/* NSH-safe single-quote wrap; reject embedded single quotes */
static int shell_quote(const char *in, char *out, size_t outsz)
{
  size_t n;
  size_t o = 0;

  if (!in || !out || outsz < 3)
    {
      return -1;
    }
  if (strchr(in, '\'') != NULL)
    {
      return -1;
    }
  n = strlen(in);
  if (n + 3 > outsz)
    {
      return -1;
    }
  out[o++] = '\'';
  memcpy(out + o, in, n);
  o += n;
  out[o++] = '\'';
  out[o] = 0;
  return 0;
}

static void cli_sta_up(void)
{
  syslog(LOG_INFO, "[deskmate-wifi] ifup + mode STA\n");
  (void)system("ifup wlan0");
  (void)system("wapi mode wlan0 2");
}

static int wifi_get_valid_ip(struct in_addr *addr)
{
  if (netlib_get_ipv4addr(DM_WIFI_IF, addr) != 0)
    {
      return -1;
    }
  if (addr->s_addr == 0 || addr->s_addr == 0xffffffffu)
    {
      return -1;
    }
  return 0;
}

static int wifi_ntp_has_samples(void)
{
  struct ntpc_status_s status;

  memset(&status, 0, sizeof(status));
  return ntpc_status(&status) == 0 && status.nsamples > 0;
}

static int wifi_clock_in_range(time_t now)
{
  struct tm utc;

  return now > 0 && gmtime_r(&now, &utc) != NULL &&
         utc.tm_year + 1900 >= 2024 && utc.tm_year + 1900 <= 2099;
}

static int wifi_clock_synced(void)
{
  int manual;

  if (!wifi_clock_in_range(time(NULL)))
    {
      return 0;
    }

  /* Manual confirmation must not wait behind ntpc_status(): the SDK holds
   * its own NTP lock during daemon stop/start. A restored file is only a
   * clock seed, never evidence of the current time after a power-off. */
  pthread_mutex_lock(&s_ntp_lock);
  manual = s_manual_time_set;
  pthread_mutex_unlock(&s_ntp_lock);
  if (manual)
    {
      return 1;
    }
  if (wifi_ntp_has_samples())
    {
      pthread_mutex_lock(&s_ntp_lock);
      s_clock_seed_only = 0;
      pthread_mutex_unlock(&s_ntp_lock);
      return 1;
    }
  return 0;
}

int dm_wifi_clock_valid(void)
{
  /* Reminders and HTTPS must agree about whether this clock is usable. */
  return wifi_clock_synced();
}

int dm_wifi_set_manual_time(time_t epoch)
{
  FILE *f;
  time_t actual;
  struct timeval tv;
  int saved = 0;

  if (!wifi_clock_in_range(epoch))
    {
      return -EINVAL;
    }
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  if (settimeofday(&tv, NULL) != 0)
    {
      int rc = errno ? -errno : -EIO;
      syslog(LOG_ERR, "[deskmate-wifi] manual clock set failed rc=%d\n", rc);
      return rc;
    }
  actual = time(NULL);
  if (actual < epoch || actual - epoch > 2)
    {
      syslog(LOG_ERR, "[deskmate-wifi] manual clock readback failed\n");
      return -EIO;
    }

  pthread_mutex_lock(&s_ntp_lock);
  s_manual_time_set = 1;
  s_clock_seed_only = 0;
  pthread_mutex_unlock(&s_ntp_lock);

  f = fopen(DM_WIFI_MANUAL_TIME_PATH, "w");
  if (f)
    {
      int wrote = fprintf(f, "%lld\n", (long long)actual) > 0;
      int closed = fclose(f) == 0;
      saved = wrote && closed;
    }
  syslog(LOG_INFO,
         "[deskmate-wifi] manual clock accepted epoch=%lld seed_saved=%d; HTTPS clock ready\n",
         (long long)actual, saved);
  return 0;
}

void dm_wifi_manual_time_restore(void)
{
  FILE *f;
  long long saved_epoch;
  time_t saved_time;
  time_t current_time = time(NULL);
  struct tm utc;
  struct timeval tv;

  /* Never replace a valid RTC or an NTP time that arrived early at boot. */
  if (current_time > 0 && gmtime_r(&current_time, &utc) != NULL &&
      utc.tm_year + 1900 >= 2024)
    {
      return;
    }

  f = fopen(DM_WIFI_MANUAL_TIME_PATH, "r");
  if (!f)
    {
      return;
    }
  if (fscanf(f, "%lld", &saved_epoch) != 1)
    {
      fclose(f);
      return;
    }
  fclose(f);

  saved_time = (time_t)saved_epoch;
  if ((long long)saved_time != saved_epoch || saved_time <= 0 ||
      gmtime_r(&saved_time, &utc) == NULL || utc.tm_year + 1900 < 2024 ||
      utc.tm_year + 1900 > 2099)
    {
      return;
    }

  tv.tv_sec = saved_time;
  tv.tv_usec = 0;
  if (settimeofday(&tv, NULL) == 0)
    {
      pthread_mutex_lock(&s_ntp_lock);
      s_manual_time_set = 0;
      s_clock_seed_only = 1;
      s_ntp_retry_after_ms = 0;
      pthread_mutex_unlock(&s_ntp_lock);
      syslog(LOG_INFO,
             "[deskmate-wifi] restored saved local clock seed; NTP may correct it\n");
    }
}

static void wifi_restart_ntp_after_link(void)
{
  int stop_rc;
  int start_rc;
  int select_servers;
  uint64_t now_ms;

  if (wifi_ntp_has_samples())
    {
      return;
    }

  pthread_mutex_lock(&s_ntp_lock);
  now_ms = wifi_monotonic_ms();
  if (s_ntp_refresh_running ||
      (s_ntp_retry_after_ms != 0 && now_ms < s_ntp_retry_after_ms))
    {
      pthread_mutex_unlock(&s_ntp_lock);
      return;
    }

  s_ntp_refresh_running = 1;
  select_servers = !s_ntp_servers_selected;
  pthread_mutex_unlock(&s_ntp_lock);

  /* Do not hold the clock-state mutex while waiting for the SDK daemon.
   * Manual calibration must remain usable even if NTP DNS is timing out.
   * Replace the stock server list once. Subsequent starts leave a running
   * daemon alone; ntpc_start_with_list() only creates one if it has exited. */
  stop_rc = select_servers ? ntpc_stop() : 0;
  start_rc = stop_rc < 0 ? stop_rc :
             ntpc_start_with_list(DM_WIFI_NTP_SERVERS);

  pthread_mutex_lock(&s_ntp_lock);
  s_ntp_refresh_running = 0;
  if (start_rc >= 0)
    {
      s_ntp_servers_selected = 1;
    }
  s_ntp_retry_after_ms = wifi_monotonic_ms() +
      (uint64_t)DM_WIFI_NTP_RETRY_SEC * 1000;
  pthread_mutex_unlock(&s_ntp_lock);
  syslog(start_rc < 0 ? LOG_WARNING : LOG_INFO,
         "[deskmate-wifi] NTP refresh stop=%d start=%d\n",
         stop_rc, start_rc);
}

int dm_wifi_wait_time_sync(int timeout_sec)
{
  struct ntpc_status_s status;
  struct tm utc;
  time_t now;
  int year = 0;
  int status_rc;
  int manual;
  int seed;
  int i;

  if (timeout_sec < 0)
    {
      timeout_sec = 0;
    }
  if (timeout_sec > 90)
    {
      timeout_sec = 90;
    }

  for (i = 0; i <= timeout_sec; i++)
    {
      if (wifi_clock_synced())
        {
          return 0;
        }
      if (!dm_wifi_connected())
        {
          return -ENETDOWN;
        }
      if (i == 0 || (i > 0 && (i % DM_WIFI_NTP_RETRY_SEC) == 0))
        {
          wifi_restart_ntp_after_link();
        }
      if (i < timeout_sec)
        {
          sleep(1);
        }
    }

  /* Catch a manual calibration arriving at the end of the wait. */
  if (wifi_clock_synced())
    {
      return 0;
    }
  pthread_mutex_lock(&s_ntp_lock);
  manual = s_manual_time_set;
  seed = s_clock_seed_only;
  pthread_mutex_unlock(&s_ntp_lock);
  memset(&status, 0, sizeof(status));
  status_rc = ntpc_status(&status);
  now = time(NULL);
  if (now > 0 && gmtime_r(&now, &utc) != NULL)
    {
      year = utc.tm_year + 1900;
    }
  syslog(LOG_WARNING,
         "[deskmate-wifi] clock not ready; status=%d samples=%u year=%d manual=%d seed_only=%d; HTTPS deferred\n",
         status_rc, status.nsamples, year, manual, seed);
  return -ETIMEDOUT;
}

/* Check wapi show wlan0 like 9.5 wifi_manager.c get_wifi_status() */
static int wifi_is_associated(char *assoc_ssid, size_t ssid_len)
{
  FILE *fp;
  char buf[256];
  int associated = 0;

  if (assoc_ssid && ssid_len > 0)
    {
      assoc_ssid[0] = 0;
    }

  if (system("wapi show wlan0 > /tmp/wifi_status_connect.txt") != 0) return 0;
  fp = fopen("/tmp/wifi_status_connect.txt", "r");
  if (!fp)
    {
      return 0;
    }

  while (fgets(buf, sizeof(buf), fp) != NULL)
    {
      buf[strcspn(buf, "\r\n")] = 0;
      if (strstr(buf, "AP:") &&
          !strstr(buf, "AP: 00:00:00:00:00:00")) associated = 1;
      if (strstr(buf, "AP: 00:00:00:00:00:00") != NULL)
        {
          associated = 0;
        }
      if (assoc_ssid && ssid_len > 0)
        {
          char *p = strstr(buf, "ESSID:");
          if (p)
            {
              p += 6;
              while (*p == ' ' || *p == '"')
                {
                  p++;
                }
              snprintf(assoc_ssid, ssid_len, "%s", p);
              assoc_ssid[strcspn(assoc_ssid, "\"\r\n")] = 0;
            }
        }
    }

  fclose(fp);
  return associated;
}

/* Verify the current association is really the expected SSID.
 * Returns 1 = match, 0 = mismatch / not associated, -1 = cannot read. */
static int wifi_verify_assoc(const char *expected)
{
  char assoc[33];

  if (!expected || !expected[0])
    {
      return -1;
    }
  if (!wifi_is_associated(assoc, sizeof(assoc)))
    {
      return 0;
    }
  if (!assoc[0])
    {
      return -1;
    }
  return strcmp(assoc, expected) == 0 ? 1 : 0;
}

/* Port of 9.5 wifi_manager.c:get_wifi_status().
 * 1 = associated, 0 = not associated (AP is all-zero), -1 = cannot read.
 * "wapi show" returns early when there is no IPv4 yet, so it is only used as
 * an EXTRA check after an address was obtained. */
static int wifi_is_link_up(void)
{
  FILE *fp;
  char buf[256];
  int associated = 0;

  if (system("wapi show wlan0 > /tmp/wifi_status.txt 2>&1") != 0) return -1;
  fp = fopen("/tmp/wifi_status.txt", "r");
  if (!fp)
    {
      return -1;
    }

  while (fgets(buf, sizeof(buf), fp) != NULL)
    {
      if (strstr(buf, "AP:") &&
          !strstr(buf, "AP: 00:00:00:00:00:00")) associated = 1;
      if (strstr(buf, "AP: 00:00:00:00:00:00") != NULL)
        {
          associated = 0;
          break;
        }
    }

  fclose(fp);
  return associated;
}

int dm_wifi_enabled(void)
{
  return s_wifi_enabled;
}

int dm_wifi_set_enabled(int on)
{
  on = on ? 1 : 0;
  if (s_wifi_enabled == on)
    {
      return 0;
    }

  s_wifi_enabled = on;
  s_link_lost = 0;

  if (on)
    {
      s_manual_override = 0;
      cli_sta_up();
      syslog(LOG_INFO, "[deskmate-wifi] enabled\n");
      wifi_request_reconnect();
    }
  else
    {
      s_manual_override = 1;
      (void)system("wapi disconnect wlan0");
      (void)system("ifdown wlan0");
      pthread_mutex_lock(&s_lock);
      s_cur_ssid[0] = 0;
      s_cur_ip[0] = 0;
      pthread_mutex_unlock(&s_lock);
      s_conn_ok = 0;
      s_conn_fail = 0;
      syslog(LOG_INFO, "[deskmate-wifi] disabled\n");
    }
  return 0;
}

int dm_wifi_link_lost(void)
{
  return s_link_lost;
}

void dm_wifi_clear_link_lost(void)
{
  s_link_lost = 0;
}

static void wifi_mark_link_lost(void)
{
  pthread_mutex_lock(&s_lock);
  s_cur_ssid[0] = 0;
  s_cur_ip[0] = 0;
  pthread_mutex_unlock(&s_lock);
  s_link_lost = 1;
  syslog(LOG_WARNING, "[deskmate-wifi] link lost\n");
  wifi_request_reconnect();
}

void dm_wifi_poll(void)
{
  struct in_addr addr;

  if (!s_wifi_enabled)
    {
      return;
    }

  s_poll_ticks++;
  if ((s_poll_ticks & 7) != 0)
    {
      return;
    }

  if (!dm_wifi_connected())
    {
      wifi_request_reconnect();
      return;
    }

  if (wifi_get_valid_ip(&addr) != 0)
    {
      wifi_mark_link_lost();
      return;
    }

  if ((s_poll_ticks & 31) == 0 && wifi_is_link_up() == 0)
    {
      wifi_mark_link_lost();
    }
}

static void wifi_cred_save(const char *ssid, const char *pass)
{
  FILE *f;

  if (!ssid || !ssid[0])
    {
      return;
    }
  /* Reference save_wifi_config(): /data/wifi.cfg */
  f = fopen("/data/wifi.cfg", "w");
  if (f)
    {
      fprintf(f, "SSID=\"%s\"\n", ssid);
      fprintf(f, "PASSWORD=\"%s\"\n", pass ? pass : "");
      fclose(f);
    }
  /* Keep deskmate's own file as a mirror */
  f = fopen(DM_WIFI_SAVE_PATH, "w");
  if (f)
    {
      fprintf(f, "%s\n%s\n", ssid, pass ? pass : "");
      fclose(f);
    }
}

static void wifi_unquote(char *s)
{
  size_t n;

  if (!s || s[0] != '"')
    {
      return;
    }
  n = strlen(s);
  if (n >= 2 && s[n - 1] == '"')
    {
      memmove(s, s + 1, n - 2);
      s[n - 2] = 0;
    }
}

static int wifi_cred_load_cfg(char *ssid, size_t slen, char *pass,
                              size_t plen)
{
  FILE *f = fopen("/data/wifi.cfg", "r");
  char line[96];
  int got_ssid = 0;
  int got_pass = 0;

  if (!f)
    {
      return -1;
    }
  while (fgets(line, sizeof(line), f))
    {
      line[strcspn(line, "\r\n")] = 0;
      if (strncmp(line, "SSID=", 5) == 0)
        {
          snprintf(ssid, slen, "%s", line + 5);
          wifi_unquote(ssid);
          got_ssid = 1;
        }
      else if (strncmp(line, "PASSWORD=", 9) == 0)
        {
          snprintf(pass, plen, "%s", line + 9);
          wifi_unquote(pass);
          got_pass = 1;
        }
    }
  fclose(f);

  if (!got_ssid || !ssid[0])
    {
      return -1;
    }
  if (!got_pass)
    {
      pass[0] = 0;
    }
  syslog(LOG_INFO, "[deskmate-wifi] creds from /data/wifi.cfg ssid=%s\n",
         ssid);
  return 0;
}

static int wifi_cred_load_wapi_json(char *ssid, size_t slen, char *pass,
                                    size_t plen)
{
  FILE *f;
  cJSON *root;
  cJSON *ifobj;
  cJSON *ssid_item;
  cJSON *psk_item;
  char *buf;
  long size;
  int ret = -1;

  f = fopen(DM_WIFI_WAPI_CONFIG_PATH, "rb");
  if (!f)
    {
      return -1;
    }

  if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 || size > 4096 ||
      fseek(f, 0, SEEK_SET) != 0)
    {
      fclose(f);
      return -1;
    }

  buf = malloc((size_t)size + 1);
  if (!buf)
    {
      fclose(f);
      return -1;
    }
  if (fread(buf, 1, (size_t)size, f) != (size_t)size)
    {
      free(buf);
      fclose(f);
      return -1;
    }
  fclose(f);
  buf[size] = '\0';

  root = cJSON_Parse(buf);
  free(buf);
  if (!root)
    {
      return -1;
    }

  /* WAPI save_config writes {"wlan0":{"ssid":...,"psk":...}}.  Accept
   * the older flat shape as a compatibility fallback. */
  ifobj = cJSON_GetObjectItem(root, DM_WIFI_IF);
  if (!ifobj)
    {
      ifobj = root;
    }
  ssid_item = cJSON_GetObjectItem(ifobj, "ssid");
  psk_item = cJSON_GetObjectItem(ifobj, "psk");
  if (ssid_item && cJSON_IsString(ssid_item) && ssid_item->valuestring &&
      ssid_item->valuestring[0])
    {
      snprintf(ssid, slen, "%s", ssid_item->valuestring);
      if (psk_item && cJSON_IsString(psk_item) && psk_item->valuestring)
        {
          snprintf(pass, plen, "%s", psk_item->valuestring);
        }
      else
        {
          pass[0] = '\0';
        }
      ret = 0;
    }

  cJSON_Delete(root);
  if (ret == 0)
    {
      syslog(LOG_INFO, "[deskmate-wifi] loaded credentials from WAPI config\n");
    }
  return ret;
}

static int wifi_cred_load(char *ssid, size_t slen, char *pass, size_t plen)
{
  FILE *f = fopen(DM_WIFI_SAVE_PATH, "r");
  char line[96];

  if (f)
    {
      if (fgets(line, sizeof(line), f))
        {
          line[strcspn(line, "\r\n")] = 0;
          if (line[0])
            {
              snprintf(ssid, slen, "%s", line);
              if (fgets(line, sizeof(line), f))
                {
                  line[strcspn(line, "\r\n")] = 0;
                  snprintf(pass, plen, "%s", line);
                }
              else
                {
                  pass[0] = '\0';
                }
              fclose(f);
              return 0;
            }
        }
      fclose(f);
    }

  if (wifi_cred_load_cfg(ssid, slen, pass, plen) == 0)
    {
      return 0;
    }

  return wifi_cred_load_wapi_json(ssid, slen, pass, plen);
}

static int ensure_driver_wait(int attempts_100ms)
{
  int i;

  if (s_drv_ready && if_nametoindex(DM_WIFI_IF) != 0)
    {
      return 0;
    }

  for (i = 0; i < attempts_100ms; i++)
    {
      if (if_nametoindex(DM_WIFI_IF) != 0)
        {
          s_drv_ready = 1;
          syslog(LOG_INFO, "[deskmate-wifi] %s ready\n", DM_WIFI_IF);
          return 0;
        }
      usleep(100 * 1000);
    }

  s_drv_ready = 0;
  syslog(LOG_ERR,
         "[deskmate-wifi] %s missing — board bringup not ready yet\n",
         DM_WIFI_IF);
  return -1;
}

static int ensure_driver(void)
{
  return ensure_driver_wait(200);
}

/* Parse /tmp/wifi_scan_result.txt:
 *   bssid / frequency / signal level / encode / ssid
 *   74:39:89:f8:f0:ae       5240    -59     0803    Programmers7
 *   d2:ad:08:f8:eb:71       2462    -88     0803    DIRECT-71-HP Smart Tank 750
 *
 * Preserves spaces inside SSIDs, skips hidden (empty) SSIDs and non-MAC lines,
 * deduplicates same-SSID entries (keeping strongest RSSI), and sorts by RSSI.
 */
static int collect_aps_from_txt(void)
{
  FILE *fp;
  char line[256];
  int n = 0;
  int i;
  int j;

  fp = fopen("/tmp/wifi_scan_result.txt", "r");
  if (!fp)
    {
      return -1;
    }

  pthread_mutex_lock(&s_lock);
  s_ap_n = 0;

  while (fgets(line, sizeof(line), fp) && n < DM_WIFI_AP_MAX)
    {
      char *p = line;
      char *tok_bssid;
      char *tok_freq;
      char *tok_rssi;
      char *tok_enc;
      char *ssid_start;
      size_t ssid_len;
      int rssi;
      bool is_open;
      int dup_idx = -1;

      line[strcspn(line, "\r\n")] = 0;

      while (*p == ' ' || *p == '\t')
        {
          p++;
        }
      if (!*p || strncmp(p, "bssid", 5) == 0)
        {
          continue;
        }

      /* Field 0: BSSID (must contain ':') */
      tok_bssid = p;
      while (*p && *p != ' ' && *p != '\t')
        {
          p++;
        }
      if (!*p)
        {
          continue;
        }
      *p++ = 0;
      if (strchr(tok_bssid, ':') == NULL)
        {
          continue;
        }

      /* Field 1: frequency */
      while (*p == ' ' || *p == '\t')
        {
          p++;
        }
      tok_freq = p;
      while (*p && *p != ' ' && *p != '\t')
        {
          p++;
        }
      if (!*p || !*tok_freq)
        {
          continue;
        }
      *p++ = 0;

      /* Field 2: signal level (RSSI) */
      while (*p == ' ' || *p == '\t')
        {
          p++;
        }
      tok_rssi = p;
      while (*p && *p != ' ' && *p != '\t')
        {
          p++;
        }
      if (!*p || !*tok_rssi)
        {
          continue;
        }
      *p++ = 0;
      rssi = atoi(tok_rssi);

      /* Field 3: encode (0000 = open, otherwise encrypted) */
      while (*p == ' ' || *p == '\t')
        {
          p++;
        }
      tok_enc = p;
      while (*p && *p != ' ' && *p != '\t')
        {
          p++;
        }
      if (*p)
        {
          *p++ = 0;
        }
      if (!*tok_enc)
        {
          continue;
        }
      is_open = (strcmp(tok_enc, "0000") == 0);

      /* Field 4+: entire remainder of line is SSID (may contain spaces) */
      while (*p == ' ' || *p == '\t')
        {
          p++;
        }
      ssid_start = p;
      ssid_len = strlen(ssid_start);
      while (ssid_len > 0 &&
             (ssid_start[ssid_len - 1] == ' ' ||
              ssid_start[ssid_len - 1] == '\t'))
        {
          ssid_start[--ssid_len] = 0;
        }
      if (ssid_len == 0)
        {
          /* Hidden AP with blank SSID */
          continue;
        }

      /* Deduplicate by SSID: keep strongest RSSI */
      for (i = 0; i < n; i++)
        {
          if (strncmp(s_aps[i].ssid, ssid_start, sizeof(s_aps[i].ssid) - 1) == 0)
            {
              dup_idx = i;
              break;
            }
        }
      if (dup_idx >= 0)
        {
          if (rssi > s_aps[dup_idx].rssi)
            {
              s_aps[dup_idx].rssi = rssi;
              s_aps[dup_idx].open = is_open;
            }
          continue;
        }

      strncpy(s_aps[n].ssid, ssid_start, sizeof(s_aps[n].ssid) - 1);
      s_aps[n].ssid[sizeof(s_aps[n].ssid) - 1] = 0;
      s_aps[n].rssi = rssi;
      s_aps[n].open = is_open;
      n++;
    }

  /* Sort APs by RSSI descending (strongest signal first) */
  for (i = 0; i < n - 1; i++)
    {
      for (j = i + 1; j < n; j++)
        {
          if (s_aps[j].rssi > s_aps[i].rssi)
            {
              dm_ap_t tmp = s_aps[i];
              s_aps[i] = s_aps[j];
              s_aps[j] = tmp;
            }
        }
    }

  s_ap_n = n;
  if (s_sel >= s_ap_n)
    {
      s_sel = (s_ap_n > 0) ? 0 : -1;
    }
  pthread_mutex_unlock(&s_lock);
  fclose(fp);
  return n;
}

static void *scan_thread(void *arg)
{
  int i;
  int n;

  (void)arg;

  if (!s_wifi_enabled)
    {
      s_scan_busy = 0;
      s_scan_ready = 1;
      return NULL;
    }

  /* Wait briefly if an auto-connect thread is yielding */
  for (i = 0; i < 50 && s_conn_busy; i++)
    {
      usleep(100 * 1000);
    }

  if (ensure_driver() < 0)
    {
      s_scan_busy = 0;
      s_scan_ready = 1;
      return NULL;
    }

  /* Only bring interface up in STA mode if not already connected */
  if (!dm_wifi_connected())
    {
      cli_sta_up();
      usleep(200 * 1000);
    }

  /* Match 9.5 demo get_wifi.sh: wapi scan wlan0 > /tmp/wifi_scan_result.txt */
  (void)system("wapi scan wlan0 > /tmp/wifi_scan_result.txt");
  usleep(300 * 1000);

  n = collect_aps_from_txt();
  if (n <= 0)
    {
      usleep(500 * 1000);
      (void)system("wapi scan wlan0 > /tmp/wifi_scan_result.txt");
      usleep(300 * 1000);
      n = collect_aps_from_txt();
    }

  syslog(LOG_INFO, "[deskmate-wifi] wapi scan -> %d aps\n", n);
  for (i = 0; i < s_ap_n; i++)
    {
      syslog(LOG_INFO, "[deskmate-wifi]  ap %s rssi=%d open=%d\n",
             s_aps[i].ssid, s_aps[i].rssi, (int)s_aps[i].open);
    }

  s_scan_busy = 0;
  s_scan_ready = 1;
  return NULL;
}

int dm_wifi_start_scan(void)
{
  pthread_t th;
  pthread_attr_t attr;
  int ret;

  if (!s_wifi_enabled)
    {
      return -1;
    }

  if (s_scan_busy)
    {
      return 0;
    }

  /* Interrupt any background boot auto-connect so manual scan is never blocked */
  s_manual_override = 1;
  if (s_conn_busy && !s_conn_is_auto)
    {
      return -1;
    }

  s_scan_ready = 0;
  s_scan_busy = 1;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, DM_WIFI_STACK);
  ret = pthread_create(&th, &attr, scan_thread, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      s_scan_busy = 0;
      return -1;
    }
  pthread_detach(th);
  return 0;
}

/* Official connect — ifup → mode 2 → psk(3 2) → essid(1) → wait → renew */
static void *conn_thread(void *arg)
{
  char cmd[192];
  char qssid[80];
  char qpass[160];
  int i;
  int attempt;
  int got_ip = 0;
  int is_auto = s_conn_is_auto;
  struct in_addr addr;
  char try_ssid[33];
  char try_pass[64];
  bool try_open;

  (void)arg;

  if (!s_wifi_enabled)
    {
      s_conn_busy = 0;
      s_conn_fail = 1;
      return NULL;
    }

  strncpy(try_ssid, s_pending_ssid, sizeof(try_ssid) - 1);
  try_ssid[sizeof(try_ssid) - 1] = 0;
  strncpy(try_pass, s_pending_pass, sizeof(try_pass) - 1);
  try_pass[sizeof(try_pass) - 1] = 0;
  try_open = s_pending_open;

  if (try_ssid[0] == 0 || if_nametoindex(DM_WIFI_IF) == 0)
    {
      syslog(LOG_ERR, "[deskmate-wifi] connect abort: no ssid/wlan0\n");
      s_conn_busy = 0;
      s_conn_fail = 1;
      return NULL;
    }

  if (ensure_driver() < 0)
    {
      s_conn_busy = 0;
      s_conn_fail = 1;
      return NULL;
    }

  if (is_auto && s_manual_override)
    {
      s_conn_busy = 0;
      return NULL;
    }

  /* NOTE: credentials are persisted only after a verified connection (below).

   * 9) The 9.5 wifi_manager retries the whole sequence every 5s until it is
   * associated. Port that behaviour here: up to DM_WIFI_CONN_ROUNDS tries. */
  for (attempt = 0; attempt < DM_WIFI_CONN_ROUNDS; attempt++)
    {
      if (attempt > 0)
        {
          syslog(LOG_INFO, "[deskmate-wifi] retry %d/%d in %ds\n",
                 attempt + 1, DM_WIFI_CONN_ROUNDS, DM_WIFI_CONN_RETRY_SEC);
          sleep(DM_WIFI_CONN_RETRY_SEC);
        }

      got_ip = 0;

      /* 1) ifup + STA mode */
      cli_sta_up();

  /* 1.5) Drop the previous association so switching between networks
   *      (WPA2 <-> open) always starts from a clean driver state. */
  syslog(LOG_INFO, "[deskmate-wifi] wapi disconnect %s\n", DM_WIFI_IF);
  (void)system("wapi disconnect wlan0");
  usleep(100 * 1000);

  /* 2) psk — WPA_ALG_CCMP = 3, WPA2 = 2 */
  if (!try_open && try_pass[0])
    {
      if (shell_quote(try_pass, qpass, sizeof(qpass)) < 0)
        {
          s_conn_busy = 0;
          s_conn_fail = 1;
          return NULL;
        }
      snprintf(cmd, sizeof(cmd), "wapi psk %s %s 3 2", DM_WIFI_IF, qpass);
      syslog(LOG_INFO,
             "[deskmate-wifi] applying WPA2 credentials (redacted)\n");
      if (system(cmd) != 0)
        {
          syslog(LOG_ERR, "[deskmate-wifi] psk failed (passphrase must be 8-63 chars)\n");
          s_conn_busy = 0;
          s_conn_fail = 1;
          return NULL;
        }
    }
  else
    {
      /* Open AP: clear any leftover WPA key (alg 0 = WPA_ALG_NONE), otherwise
       * the driver keeps the previous CCMP key and cannot join open networks. */
      syslog(LOG_INFO, "[deskmate-wifi] open AP: clear psk\n");
      (void)system("wapi psk wlan0 00000000 0");
    }

  /* 3) essid — flag 1 = connect */
  if (shell_quote(try_ssid, qssid, sizeof(qssid)) < 0)
    {
      s_conn_busy = 0;
      s_conn_fail = 1;
      return NULL;
    }
  snprintf(cmd, sizeof(cmd), "wapi essid %s %s 1", DM_WIFI_IF, qssid);
  syslog(LOG_INFO, "[deskmate-wifi] %s\n", cmd);
  if (system(cmd) != 0)
    {
      syslog(LOG_ERR, "[deskmate-wifi] essid join failed\n");
      s_conn_busy = 0;
      s_conn_fail = 1;
      return NULL;
    }

  /* 4) Wait for WPA2 4-way handshake / association (interruptible if auto) */
  for (i = 0; i < 25; i++)
    {
      if (!s_wifi_enabled)
        {
          s_conn_busy = 0;
          return NULL;
        }
      if (is_auto && s_manual_override)
        {
          s_conn_busy = 0;
          return NULL;
        }
      usleep(100 * 1000);
    }

  /* 5) DHCP renew — single DHCP call per attempt */
  for (i = 0; i < 4; i++)
    {
      if (is_auto && s_manual_override)
        {
          s_conn_busy = 0;
          return NULL;
        }

      (void)system("renew wlan0");
      if (wifi_get_valid_ip(&addr) == 0)
        {
          got_ip = 1;
          break;
        }

      if (is_auto && s_manual_override)
        {
          s_conn_busy = 0;
          return NULL;
        }

      if (netlib_obtain_ipv4addr(DM_WIFI_IF) >= 0 &&
          wifi_get_valid_ip(&addr) == 0)
        {
          got_ip = 1;
          break;
        }

      syslog(LOG_WARNING, "[deskmate-wifi] renew retry %d\n", i + 1);
      sleep(1);
    }

  /* Verify we are really associated with the intended SSID (an old IPv4
   * address may still be configured while switching networks). */
  if (got_ip)
    {
      int assoc = wifi_verify_assoc(try_ssid);
      if (assoc == 0)
        {
          syslog(LOG_ERR,
                 "[deskmate-wifi] associated to another SSID, treat as failure\n");
          got_ip = 0;
        }
      else if (assoc < 0)
        {
          syslog(LOG_WARNING,
                 "[deskmate-wifi] cannot verify ESSID, assume connected\n");
        }
    }

      /* Ported check: "wapi show" reports AP all-zero when not associated. */
      if (got_ip && wifi_is_link_up() == 0)
        {
          syslog(LOG_WARNING, "[deskmate-wifi] wapi show: not associated yet\n");
          got_ip = 0;
        }

      if (got_ip)
        {
          break;
        }

      syslog(LOG_WARNING, "[deskmate-wifi] attempt %d/%d failed\n",
             attempt + 1, DM_WIFI_CONN_ROUNDS);
    }

  if (got_ip && !s_wifi_enabled)
    {
      got_ip = 0;
    }

  /* Fix: Only set s_cur_ssid when got_ip is true! */
  pthread_mutex_lock(&s_lock);
  if (got_ip)
    {
      strncpy(s_cur_ssid, try_ssid, sizeof(s_cur_ssid) - 1);
      s_cur_ssid[sizeof(s_cur_ssid) - 1] = 0;
      strncpy(s_cur_ip, inet_ntoa(addr), sizeof(s_cur_ip) - 1);
      s_cur_ip[sizeof(s_cur_ip) - 1] = 0;
    }
  else
    {
      s_cur_ssid[0] = 0;
      s_cur_ip[0] = 0;
    }
  pthread_mutex_unlock(&s_lock);

  if (!got_ip)
    {
      syslog(LOG_ERR,
             "[deskmate-wifi] connect fail after %d attempts "
             "(ssid=%s wlan0=%d)\n",
             DM_WIFI_CONN_ROUNDS, try_ssid,
             (int)if_nametoindex(DM_WIFI_IF));
      s_conn_busy = 0;
      s_conn_fail = 1;
      return NULL;
    }

  syslog(LOG_INFO, "[deskmate-wifi] connected ssid=%s ip=%s\n",
         s_cur_ssid, s_cur_ip);

  /* FlowDesk owns credential persistence; let NTP start only after WLAN
   * has a verified association and an IPv4 address. */
  wifi_cred_save(try_ssid, try_pass);
  s_manual_override = 0;
  s_link_lost = 0;
  if (!wifi_ntp_has_samples())
    {
      wifi_restart_ntp_after_link();
    }

  s_conn_busy = 0;
  s_conn_ok = 1;
  return NULL;
}

static int wifi_start_connect_internal(const char *ssid, const char *pass,
                                       int is_auto)
{
  pthread_t th;
  pthread_attr_t attr;
  int ret;
  int i;

  if (!ssid || !ssid[0])
    {
      return -1;
    }

  if (!is_auto)
    {
      s_manual_override = 1;
      for (i = 0; i < 50 && (s_scan_busy || (s_conn_busy && s_conn_is_auto)); i++)
        {
          usleep(100 * 1000);
        }
    }

  if (s_scan_busy || s_conn_busy)
    {
      return -1;
    }

  strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);
  s_pending_ssid[sizeof(s_pending_ssid) - 1] = 0;
  if (pass)
    {
      strncpy(s_pending_pass, pass, sizeof(s_pending_pass) - 1);
      s_pending_pass[sizeof(s_pending_pass) - 1] = 0;
    }
  else
    {
      s_pending_pass[0] = 0;
    }
  s_pending_open = (pass == NULL || s_pending_pass[0] == 0);

  s_conn_ok = 0;
  s_conn_fail = 0;
  s_conn_is_auto = is_auto;
  s_conn_busy = 1;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, DM_WIFI_STACK);
  ret = pthread_create(&th, &attr, conn_thread, NULL);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      s_conn_busy = 0;
      return -1;
    }
  pthread_detach(th);
  return 0;
}

int dm_wifi_connect(const char *ssid, const char *pass)
{
  return wifi_start_connect_internal(ssid, pass, 0);
}

static void *reconnect_thread(void *arg)
{
  char ssid[33];
  char pass[64];
  int attempt;
  int i;

  (void)arg;
  ssid[0] = '\0';
  pass[0] = '\0';
  if (wifi_cred_load(ssid, sizeof(ssid), pass, sizeof(pass)) != 0 ||
      !ssid[0])
    {
      syslog(LOG_WARNING, "[deskmate-wifi] reconnect skipped: no saved credentials\n");
      goto done;
    }

  for (attempt = 0; attempt < 4 && s_wifi_enabled; attempt++)
    {
      if (dm_wifi_connected() || s_manual_override)
        {
          break;
        }

      if (attempt > 0)
        {
          sleep(DM_WIFI_CONN_RETRY_SEC);
        }
      if (s_scan_busy || s_conn_busy)
        {
          continue;
        }

      syslog(LOG_INFO, "[deskmate-wifi] saved-network reconnect attempt %d\n",
             attempt + 1);
      if (wifi_start_connect_internal(ssid, pass, 1) != 0)
        {
          continue;
        }

      for (i = 0; i < 600 && s_conn_busy && s_wifi_enabled &&
                  !s_manual_override; i++)
        {
          usleep(100 * 1000);
        }
      if (dm_wifi_connected())
        {
          break;
        }
    }

done:
  s_reconnect_running = 0;
  s_reconnect_retry_after_ms = wifi_monotonic_ms() + 30000;
  return NULL;
}

static void wifi_request_reconnect(void)
{
  pthread_t th;
  pthread_attr_t attr;

  uint64_t now_ms = wifi_monotonic_ms();

  if (!s_wifi_enabled || s_manual_override || dm_wifi_connected() ||
      s_reconnect_running || s_auto_thread_active || s_scan_busy || s_conn_busy ||
      (s_reconnect_retry_after_ms != 0 &&
       now_ms < s_reconnect_retry_after_ms))
    {
      return;
    }

  s_reconnect_running = 1;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, DM_WIFI_STACK);
  if (pthread_create(&th, &attr, reconnect_thread, NULL) == 0)
    {
      pthread_detach(th);
    }
  else
    {
      s_reconnect_running = 0;
    }
  pthread_attr_destroy(&attr);
}

/* Boot auto-connect: checks if already connected, otherwise tries saved
 * credentials once or twice and immediately yields if user opens manual scan. */
static void *auto_thread(void *arg)
{
  char ssid[33];
  char pass[64];
  struct in_addr addr;
  int round;
  int i;

  (void)arg;

  ssid[0] = 0;
  pass[0] = 0;
  if (wifi_cred_load(ssid, sizeof(ssid), pass, sizeof(pass)) != 0 ||
      !ssid[0])
    {
      if (DM_WIFI_AUTO_SSID[0] == 0)
        {
          syslog(LOG_INFO,
                 "[deskmate-wifi] no saved credentials; skip boot connect\n");
          goto done;
        }
      snprintf(ssid, sizeof(ssid), "%s", DM_WIFI_AUTO_SSID);
      snprintf(pass, sizeof(pass), "%s", DM_WIFI_AUTO_PSK);
    }

  syslog(LOG_INFO,
         "[deskmate-wifi] auto-connect ssid=%s — wait wlan0 first\n", ssid);

  for (round = 0; round < 2 && !s_manual_override; round++)
    {
      if (ensure_driver_wait(300) < 0)
        {
          if (s_manual_override)
            {
              goto done;
            }
          sleep(2);
          continue;
        }

      /* Ignore placeholder addresses unless association to the saved AP is
       * verified; wlan0 may report 10.0.0.2 while its AP MAC is still zero. */
      if (wifi_get_valid_ip(&addr) == 0 &&
          wifi_verify_assoc(ssid) == 1 && wifi_is_link_up() == 1)
        {
          pthread_mutex_lock(&s_lock);
          snprintf(s_cur_ssid, sizeof(s_cur_ssid), "%s", ssid);
          snprintf(s_cur_ip, sizeof(s_cur_ip), "%s", inet_ntoa(addr));
          pthread_mutex_unlock(&s_lock);
          syslog(LOG_INFO, "[deskmate-wifi] already connected ssid=%s ip=%s\n",
                 s_cur_ssid, s_cur_ip);
          wifi_cred_save(ssid, pass);
          if (!wifi_ntp_has_samples())
            {
              wifi_restart_ntp_after_link();
            }
          goto done;
        }

      if (dm_wifi_connected() || s_manual_override)
        {
          goto done;
        }

      dm_wifi_clear_conn_flags();
      if (wifi_start_connect_internal(ssid, pass, 1) != 0)
        {
          sleep(2);
          continue;
        }

      /* Wait for conn_thread to finish or manual override */
      for (i = 0; i < 600 && s_conn_busy && !s_manual_override; i++)
        {
          usleep(100 * 1000);
        }

      if (dm_wifi_connected())
        {
          syslog(LOG_INFO, "[deskmate-wifi] auto connected ssid=%s ip=%s\n",
                 dm_wifi_cur_ssid(), dm_wifi_cur_ip());
          dm_wifi_clear_conn_flags();
          goto done;
        }

      dm_wifi_clear_conn_flags();
      if (s_manual_override)
        {
          goto done;
        }
      sleep(2);
    }

  syslog(LOG_INFO,
         "[deskmate-wifi] auto-connect finished; manual UI ready\n");
done:
  s_auto_thread_active = 0;
  return NULL;
}

/* Boot auto-connect: reads saved credentials (/data/deskmate_wifi.txt or
 * /data/wifi.cfg) and reconnects, yielding to the manual UI if the user
 * starts a scan/connect first.  Called once from deskmate_main.c. */
void dm_wifi_auto_start(void)
{
  pthread_t th;
  pthread_attr_t attr;

  if (s_auto_started)
    {
      return;
    }
  s_auto_started = 1;
  s_auto_thread_active = 1;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, DM_WIFI_STACK);
  if (pthread_create(&th, &attr, auto_thread, NULL) == 0)
    {
      pthread_detach(th);
    }
  else
    {
      s_auto_thread_active = 0;
    }
  pthread_attr_destroy(&attr);
}

#endif /* CONFIG_DESKMATE_APP */
