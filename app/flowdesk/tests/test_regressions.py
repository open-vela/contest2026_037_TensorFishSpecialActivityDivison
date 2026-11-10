"""Host regression tests. Run with Python 3 and GCC on Linux/WSL."""
import pathlib
import os
import re
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
AUDIT_STUB = r'''
#include "dm_ai_log.h"
#include "dm_ai_local_read.h"
#include "dm_ai_data.h"
static int audit_calls,audit_seconds,audit_rc;
static dm_ai_log_action_t audit_action;
static dm_ai_log_result_t audit_result;
static dm_ai_log_reason_t audit_reason;
static dm_ai_permission_t audit_permission;
void dm_ai_log_add(dm_ai_log_action_t a,dm_ai_log_result_t r,
                   dm_ai_log_reason_t reason,dm_ai_permission_t permission,
                   int seconds,int rc)
{audit_calls++;audit_action=a;audit_result=r;audit_reason=reason;
audit_permission=permission;audit_seconds=seconds;audit_rc=rc;}
'''
HEADERS = """
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <syslog.h>
"""


def source(name):
    return (ROOT / name).read_text(encoding="utf-8")


def function(text, name):
    match = re.search(r"^(?:static\s+)?[\w *]+\b" + name +
                      r"\([^;]*?\)\s*\{", text, re.M)
    if not match:
        raise AssertionError("Function not found: " + name)
    start = text.index("{", match.start())
    # Strip comments and quoted literals for brace matching only.
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|'
                        r"'(?:\\.|[^'\\])*'|[{}]", re.S)
    depth = 0
    for token in tokens.finditer(text, start):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return text[match.start():token.end()]
    raise AssertionError("Unbalanced function: " + name)


class Regressions(unittest.TestCase):
    def test_public_source_contains_no_embedded_credentials(self):
        for name in ("dm_voice.c", "dm_chat_ai.c"):
            text = source(name)
            self.assertNotIn("MIMO_EMBEDDED_KEY", text)
            self.assertNotRegex(text, r"(?:tp-|sk-|ghp_|github_pat_)[A-Za-z0-9_-]{20,}")

    def test_runtime_key_loaders_require_valid_device_file(self):
        self.run_c(r'''
#define MIMO_KEY_PATH "device-mimo.key"
''' + function(source("dm_voice.c"), "voice_key_load") + "\n" +
                   function(source("dm_chat_ai.c"), "chat_key_load") + r'''
int main(void){char key[128];FILE*f;
int(*loaders[])(char*,size_t)={voice_key_load,chat_key_load};
for(unsigned i=0;i<2;i++){
remove(MIMO_KEY_PATH);memset(key,'X',sizeof(key));
assert(!loaders[i](key,sizeof(key)) && key[0]==0);
assert(!loaders[i](NULL,sizeof(key)));
f=fopen(MIMO_KEY_PATH,"w");fputs("test_dummy_device_credential_123456\r\n",f);fclose(f);
assert(loaders[i](key,sizeof(key)) && !strcmp(key,"test_dummy_device_credential_123456"));
f=fopen(MIMO_KEY_PATH,"w");fputs("invalid key with spaces",f);fclose(f);
assert(!loaders[i](key,sizeof(key)) && key[0]==0);
f=fopen(MIMO_KEY_PATH,"w");fputs("short",f);fclose(f);
assert(!loaders[i](key,sizeof(key)) && key[0]==0);
f=fopen(MIMO_KEY_PATH,"w");for(int n=0;n<200;n++)fputc('A',f);fclose(f);
assert(!loaders[i](key,sizeof(key)) && key[0]==0);
}return 0;}
''')

    def run_c(self, code):
        with tempfile.TemporaryDirectory(prefix="flowdesk-test-") as directory:
            path = pathlib.Path(directory)
            (path / "test.c").write_text(HEADERS + code, encoding="utf-8")
            compiled = subprocess.run(
                ["gcc", "-std=gnu11", "-Wall", "-Wextra",
                 "-Werror=implicit-function-declaration", "-I" + str(ROOT), str(path / "test.c"),
                 "-o", str(path / "test"), "-pthread"],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(path / "test")], cwd=directory,
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

    def parser(self):
        text = source("dm_assistant.c")
        part = text[text.index("#define INTENT_TEXT_MAX"):
                    text.index("static dm_ai_permission_t intent_permission(")]
        pages = sorted(set(re.findall(r"\bPAGE_\w+\b", part)))
        return (AUDIT_STUB + "typedef enum {" + ",".join(pages) + "} dm_page_t;\n" +
                'int dm_med_voice_has_name(const char *s) '
                '{return strstr(s,"布洛芬") != NULL;}\n' + part)

    def data_helpers(self):
        text = source("dm_ai_data.c")
        return '#include "dm_ai_data.h"\n' + "\n".join(function(text, name) for name in (
            "dm_ai_data_read_permission", "dm_ai_data_delete_permission",
            "dm_ai_data_delete_allowed", "dm_ai_data_match")) + "\n"

    def test_health_questions_are_local_and_deletes_are_exact_commands(self):
        self.run_c(self.parser() + r'''
int main(void){assistant_intent_t i;
assert(parse_intent("喝了多少杯水",&i) && i.type==INTENT_READ_HEALTH && i.health_query==DM_AI_HEALTH_WATER);
assert(parse_intent("我今天喝了几杯水？",&i) && i.type==INTENT_READ_HEALTH);
assert(parse_intent("最近一条心情是什么时候",&i) && i.type==INTENT_READ_HEALTH && i.health_query==DM_AI_HEALTH_LAST_MOOD);
assert(parse_intent("昨天喝了多少杯水",&i) && i.health_query==DM_AI_HEALTH_UNAVAILABLE);
assert(parse_intent("我喝了多少毫升水",&i) && i.health_query==DM_AI_HEALTH_UNAVAILABLE);
assert(parse_intent("我上次记录心情是什么时候？",&i) && i.type==INTENT_READ_HEALTH);
assert(parse_intent("今天喝了多少杯水，还有最近一条心情是什么时候",&i) && i.type==INTENT_READ_HEALTH && i.health_query==DM_AI_HEALTH_WATER_AND_MOOD);
assert(parse_intent("删除第2条备忘",&i) && i.type==INTENT_DELETE_DATA && i.deletion.kind==DM_AI_DATA_NOTE && i.deletion.number==2);
assert(parse_intent("删除待办‘开会’",&i) && i.type==INTENT_DELETE_DATA && i.deletion.kind==DM_AI_DATA_TODO && !strcmp(i.deletion.title,"开会"));
assert(parse_intent("请帮我删除闹钟\"起床\"",&i) && i.type==INTENT_DELETE_DATA && i.deletion.kind==DM_AI_DATA_ALARM);
assert(parse_intent("删除倒数日“生日”",&i) && i.type==INTENT_DELETE_DATA && i.deletion.kind==DM_AI_DATA_COUNTDOWN);
assert(parse_intent("删除最近一条心情记录",&i) && i.type==INTENT_DELETE_DATA && i.deletion.selector==DM_AI_SELECT_LATEST);
assert(parse_intent("删除今天2杯喝水记录",&i) && i.type==INTENT_DELETE_DATA && i.deletion.number==2 && i.deletion.selector==DM_AI_SELECT_CUPS);
assert(parse_intent("删除今天的喝水记录",&i) && i.type==INTENT_DELETE_DATA && i.deletion.selector==DM_AI_SELECT_TODAY);
assert(parse_intent("删除服药记录‘维生素’",&i) && i.type==INTENT_DELETE_DATA && i.deletion.kind==DM_AI_DATA_MEDICATION);
assert(parse_intent("删除今天维生素的服药记录",&i) && i.type==INTENT_DELETE_DATA && !strcmp(i.deletion.title,"维生素"));
const char*invalid[]={"删除备忘","删除那个闹钟","删除所有备忘","清空心情记录","删除昨天的喝水记录",
"删除今天第2条喝水记录","删除今天的备忘买牛奶","删除第1条备忘和第2条待办","如果删除第1条备忘",
"可以删除第1条备忘吗","删除待办“开会”和“吃饭”","删除第99条备忘","删除今天2杯喝水记录和备忘"};
for(unsigned n=0;n<sizeof(invalid)/sizeof(invalid[0]);n++){
if(!parse_intent(invalid[n],&i) || i.type!=INTENT_DELETE_UNSUPPORTED){fprintf(stderr,"Unexpected deletion: %s type=%d\n",invalid[n],i.type);assert(0);}}
const char*negative[]={"不要删除第1条备忘","别清空今天喝水记录","我不想删除第1条备忘","不能删除最近一条心情记录"};
for(unsigned n=0;n<sizeof(negative)/sizeof(negative[0]);n++){
assert(parse_intent(negative[n],&i) && i.type==INTENT_CANCEL);}
assert(parse_intent("帮我记一下明天删除文件",&i) && i.type==INTENT_NOTE);
return 0;}
''')

    def test_health_queries_read_real_values_and_revoke_before_return(self):
        h = source("deskmate.h")
        records = h[h.index("#define DM_M_JOY"):h.index("extern dm_ctx_t g_dm;")]
        health = source("dm_ui_health.c")
        state = health[health.index("typedef struct"):health.index("static lv_obj_t *s_page;")]
        self.run_c('#include "dm_ai_data.h"\n' + records + state + r'''
static bool allowed,clock_ready=true,revoke_during_read;static int reads,s_health_day_key=123;
bool dm_ai_permission_allowed(dm_ai_permission_t p){(void)p;return allowed;}
static int dm_wifi_clock_valid(void){return clock_ready;}
static int health_today_key(void){return 123;}
int dm_water_ai_cups(int *n){reads++;*n=3;if(revoke_during_read)allowed=false;return 0;}
''' + function(health,"dm_health_ai_read") + "\n" +
                   function(source("dm_ai_data.c"),"dm_ai_read_health") + r'''
int main(void){char out[512]="OLD_SECRET";
assert(dm_ai_read_health(DM_AI_HEALTH_WATER,out,sizeof(out))==-2 && !out[0] && !reads);
allowed=true;assert(!dm_ai_read_health(DM_AI_HEALTH_WATER,out,sizeof(out)));
assert(strstr(out,"3杯") && reads==1);
s_rec_n=1;s_recs[0].hour=17;s_recs[0].min=23;s_recs[0].quick=1;
assert(!dm_ai_read_health(DM_AI_HEALTH_LAST_MOOD,out,sizeof(out)));
assert(strstr(out,"17:23") && strstr(out,"今天心情不错"));
assert(!dm_ai_read_health(DM_AI_HEALTH_WATER_AND_MOOD,out,sizeof(out)));
assert(strstr(out,"3杯") && strstr(out,"17:23"));
s_rec_n=0;assert(!dm_ai_read_health(DM_AI_HEALTH_LAST_MOOD,out,sizeof(out)));
assert(strstr(out,"没有保存") && !strstr(out,"17:23"));
s_rec_n=1;s_health_day_key=122;
assert(!dm_ai_read_health(DM_AI_HEALTH_LAST_MOOD,out,sizeof(out)) && strstr(out,"没有保存"));
clock_ready=false;assert(dm_ai_read_health(DM_AI_HEALTH_WATER,out,sizeof(out))==-4);
clock_ready=true;revoke_during_read=true;assert(dm_ai_read_health(DM_AI_HEALTH_WATER,out,sizeof(out))==-2 && !out[0]);
return 0;}
''')

    def test_note_delete_requires_read_and_delete_and_rolls_back(self):
        self.run_c(self.data_helpers() + r'''
#define NOTE_MAX 8
#define NOTE_LEN_MAX 32
static char s_notes[8][32];static int s_note_n=3,s_note_sel=2,saves,paints,save_rc;
static bool read_allowed=true,delete_allowed=true,master=true;
bool dm_ai_permission_allowed(dm_ai_permission_t p)
{return master && (p==DM_AI_READ_LOCAL_DATA?read_allowed:p==DM_AI_DELETE_LOCAL_DATA?delete_allowed:p==DM_AI_OPEN_APPS);}
static int note_save(void){saves++;return save_rc;}
static void note_paint(void){paints++;}
''' + function(source("dm_ui_note.c"),"dm_note_ai_delete") + r'''
int main(void){dm_ai_delete_request_t r={.kind=DM_AI_DATA_NOTE,.selector=DM_AI_SELECT_TITLE};
strcpy(s_notes[0],"买牛奶");strcpy(s_notes[1],"开会");strcpy(s_notes[2],"开会");strcpy(r.title,"开会");
assert(dm_note_ai_delete(&r)==-6 && !saves && s_note_n==3);
r.selector=DM_AI_SELECT_INDEX;r.number=2;read_allowed=false;
assert(dm_note_ai_delete(&r)==-2 && !saves && s_note_n==3);
read_allowed=true;delete_allowed=false;assert(dm_note_ai_delete(&r)==-2 && !saves);
delete_allowed=true;master=false;assert(dm_note_ai_delete(&r)==-2 && !saves);master=true;
save_rc=-1;assert(dm_note_ai_delete(&r)==-3 && s_note_n==3 && s_note_sel==2 && !paints);
assert(!strcmp(s_notes[0],"买牛奶") && !strcmp(s_notes[2],"开会"));
save_rc=0;assert(!dm_note_ai_delete(&r) && s_note_n==2 && paints==1);
assert(!strcmp(s_notes[1],"开会") && !s_notes[2][0]);r.number=8;
assert(dm_note_ai_delete(&r)==-5 && s_note_n==2);return 0;}
''')

    def test_delete_controller_checks_both_permissions_for_every_category(self):
        implementation = source("dm_ai_data.c").replace("#include <nuttx/config.h>", "")
        self.run_c('#define CONFIG_DESKMATE_APP 1\n' + implementation + r'''
static bool master=true,read_allowed=true,delete_allowed=true,open_allowed=true;static int calls,kind_seen;
bool dm_ai_permission_allowed(dm_ai_permission_t p)
{if(p<0||p>=DM_AI_PERMISSION_COUNT)return false;
return master && (p==DM_AI_OPEN_APPS?open_allowed:(p==DM_AI_READ_HEALTH||p==DM_AI_READ_LOCAL_DATA)?read_allowed:delete_allowed);}
static int operation(const dm_ai_delete_request_t*r){calls++;kind_seen=r->kind;return 0;}
int dm_note_ai_delete(const dm_ai_delete_request_t*r){return operation(r);}
int dm_reminder_ai_delete(const dm_ai_delete_request_t*r){return operation(r);}
int dm_countdown_ai_delete(const dm_ai_delete_request_t*r){return operation(r);}
int dm_health_ai_delete(const dm_ai_delete_request_t*r){return operation(r);}
int dm_water_ai_delete(const dm_ai_delete_request_t*r){return operation(r);}
int dm_med_ai_delete(const dm_ai_delete_request_t*r){return operation(r);}
int dm_health_ai_read(dm_ai_health_query_t q,char*out,size_t cap)
{(void)q;(void)out;(void)cap;return 0;}
int main(void){dm_ai_delete_request_t r;
for(int k=0;k<DM_AI_DATA_COUNT;k++){
memset(&r,0,sizeof(r));r.kind=k;r.selector=k==DM_AI_DATA_WATER?DM_AI_SELECT_TODAY:DM_AI_SELECT_INDEX;r.number=1;
read_allowed=false;calls=0;assert(dm_ai_delete_data(&r)==-2 && !calls);
read_allowed=true;delete_allowed=false;assert(dm_ai_delete_data(&r)==-2 && !calls);
delete_allowed=true;master=false;assert(dm_ai_delete_data(&r)==-2 && !calls);
master=true;open_allowed=false;assert(dm_ai_delete_data(&r)==-2 && !calls);
open_allowed=true;assert(!dm_ai_delete_data(&r) && calls==1 && kind_seen==k);}
r.kind=DM_AI_DATA_NOTE;r.selector=DM_AI_SELECT_INDEX;r.number=0;
calls=0;assert(dm_ai_delete_data(&r)==-1 && !calls);
r.selector=DM_AI_SELECT_TITLE;memset(r.title,'A',sizeof(r.title));
assert(dm_ai_delete_data(&r)==-1 && !calls);r.selector=DM_AI_SELECT_TODAY;
assert(dm_ai_delete_data(&r)==-1 && !calls);return 0;}
''')

    def test_reminder_delete_uses_visible_indices_and_preserves_active_alert(self):
        text = source("dm_ui_remind.c")
        types = text[text.index("typedef enum"):text.index("typedef struct\n{\n  int is_alarm;")]
        self.run_c(self.data_helpers() + '#define REMINDER_TEXT_MAX 64\n' + types + r'''
#define REMINDER_MAX 12
static reminder_t s_items[12];static int s_count=4,s_alert_idx=2,s_todo_sel,s_alarm_sel;
static int save_rc,saves,paints;static bool allowed=true;
bool dm_ai_permission_allowed(dm_ai_permission_t p){(void)p;return allowed;}
static int reminder_save(void){saves++;return save_rc;}
static void reminder_paint(void){paints++;}
''' + function(text,"dm_reminder_ai_delete") + r'''
int main(void){dm_ai_delete_request_t r={.kind=DM_AI_DATA_TODO,.selector=DM_AI_SELECT_INDEX,.number=1};
strcpy(s_items[0].text,"completed");s_items[0].fired=1;
strcpy(s_items[1].text,"task");strcpy(s_items[2].text,"ringing");
s_items[3].type=REMINDER_ALARM;strcpy(s_items[3].text,"alarm");
allowed=false;assert(dm_reminder_ai_delete(&r)==-2 && !saves);allowed=true;
save_rc=-1;assert(dm_reminder_ai_delete(&r)==-3 && s_count==4 && s_alert_idx==2 && !paints);
assert(!strcmp(s_items[1].text,"task"));save_rc=0;
assert(!dm_reminder_ai_delete(&r) && s_count==3 && s_alert_idx==1);
assert(dm_reminder_ai_delete(&r)==-7 && s_count==3);
r.kind=DM_AI_DATA_ALARM;assert(!dm_reminder_ai_delete(&r) && s_count==2);
assert(!strcmp(s_items[s_alert_idx].text,"ringing"));return 0;}
''')

    def test_countdown_delete_rolls_back_and_preserves_alert_index(self):
        self.run_c(self.data_helpers() + r'''
#define CD_MAX 8
typedef struct{char name[48];int y,m,d,hour,minute,has_alarm;} cd_item_t;
static cd_item_t s_cd[8];static int s_cd_n=3,s_cd_sel=2,s_cd_alert_idx=2,save_rc,saves;
static bool allowed=true;
bool dm_ai_permission_allowed(dm_ai_permission_t p){(void)p;return allowed;}
static int cd_save(void){saves++;return save_rc;}
static void cd_paint(void){}
''' + function(source("dm_ui_life.c"),"dm_countdown_ai_delete") + r'''
int main(void){dm_ai_delete_request_t r={.kind=DM_AI_DATA_COUNTDOWN,.selector=DM_AI_SELECT_INDEX,.number=1};
strcpy(s_cd[0].name,"first");strcpy(s_cd[1].name,"second");strcpy(s_cd[2].name,"ringing");
allowed=false;assert(dm_countdown_ai_delete(&r)==-2 && !saves);allowed=true;
save_rc=-1;assert(dm_countdown_ai_delete(&r)==-3 && s_cd_n==3 && s_cd_sel==2 && s_cd_alert_idx==2);
assert(!strcmp(s_cd[0].name,"first"));save_rc=0;
assert(!dm_countdown_ai_delete(&r) && s_cd_n==2 && s_cd_alert_idx==1);
r.number=2;assert(dm_countdown_ai_delete(&r)==-7);
assert(!strcmp(s_cd[1].name,"ringing"));return 0;}
''')

    def test_health_delete_does_not_read_when_denied_or_remove_other_statistics(self):
        h = source("deskmate.h")
        records = h[h.index("#define DM_M_JOY"):h.index("extern dm_ctx_t g_dm;")]
        self.run_c(self.data_helpers() + records + r'''
static dm_mood_rec_t s_recs[8];static int s_rec_n=2,s_health_day_key=123,saves,save_rc;
static bool allowed=true,clock_ready=true;
bool dm_ai_permission_allowed(dm_ai_permission_t p){(void)p;return allowed;}
static int dm_wifi_clock_valid(void){return clock_ready;}
static int health_today_key(void){return 123;}
static int store_save(void){saves++;return save_rc;}
static void dm_health_refresh(void){}
''' + function(source("dm_ui_health.c"),"dm_health_ai_delete") + r'''
int main(void){dm_ai_delete_request_t r={.kind=DM_AI_DATA_MOOD,.selector=DM_AI_SELECT_LATEST};
s_recs[0].hour=18;s_recs[1].hour=11;
allowed=false;assert(dm_health_ai_delete(&r)==-2 && !saves);allowed=true;
clock_ready=false;assert(dm_health_ai_delete(&r)==-4 && !saves);clock_ready=true;
save_rc=-1;assert(dm_health_ai_delete(&r)==-3 && s_rec_n==2 && s_recs[0].hour==18);
save_rc=0;assert(!dm_health_ai_delete(&r) && s_rec_n==1 && s_recs[0].hour==11);
s_health_day_key=122;assert(dm_health_ai_delete(&r)==-5 && s_rec_n==1);
return 0;}
''')

    def test_water_delete_is_atomic_and_does_not_invent_per_cup_history(self):
        text = source("dm_ui_life.c")
        self.run_c(self.data_helpers() + r'''
#include <sys/stat.h>
#define WATER_PATH "water.txt"
static int s_w_cups=4,s_w_interval=45,s_w_left=50,s_w_day=123,paints;
static time_t s_w_due;static bool s_w_alert_on,allowed=true,clock_ready=true;
bool dm_ai_permission_allowed(dm_ai_permission_t p){(void)p;return allowed;}
static int dm_wifi_clock_valid(void){return clock_ready;}
static int today_yday(void){return 123;}
static void water_paint(void){paints++;}
static void dm_health_refresh(void){}
''' + function(text,"water_save") + "\n" + function(text,"dm_water_ai_cups") + "\n" +
                   function(text,"dm_water_ai_delete") + r'''
int main(void){int cups=999;dm_ai_delete_request_t r={.kind=DM_AI_DATA_WATER,.selector=DM_AI_SELECT_CUPS,.number=2};
allowed=false;assert(dm_water_ai_cups(&cups)==-2 && cups==999);
assert(dm_water_ai_delete(&r)==-2 && s_w_cups==4);allowed=true;
FILE*f=fopen(WATER_PATH,"w");fputs("ORIGINAL",f);fclose(f);
assert(mkdir(WATER_PATH ".tmp",0700)==0);
assert(dm_water_ai_delete(&r)==-3 && s_w_cups==4 && !paints);
char saved[32];f=fopen(WATER_PATH,"r");assert(fgets(saved,sizeof(saved),f));fclose(f);
assert(!strcmp(saved,"ORIGINAL"));assert(rmdir(WATER_PATH ".tmp")==0);
r.selector=DM_AI_SELECT_INDEX;r.number=1;assert(dm_water_ai_delete(&r)==-8 && s_w_cups==4);
r.selector=DM_AI_SELECT_CUPS;r.number=5;assert(dm_water_ai_delete(&r)==-5);
r.number=2;assert(!dm_water_ai_delete(&r) && s_w_cups==2);
s_w_day=122;assert(!dm_water_ai_cups(&cups) && cups==0);
assert(dm_water_ai_delete(&r)==-5 && s_w_cups==2);s_w_day=123;
r.selector=DM_AI_SELECT_TODAY;assert(!dm_water_ai_delete(&r) && s_w_cups==0);
return 0;}
''')

    def test_medication_delete_only_clears_named_drugs_today_checkins(self):
        text = source("dm_ui_med.c")
        record = re.search(r"typedef struct\s*\{[^}]+\}\s*med_item_t;", text).group()
        self.run_c(self.data_helpers() + '#define MED_NAME_MAX 32\n' + record + r'''
static med_item_t s_med[8];static int s_med_n=2,save_rc,saves;
static bool allowed=true;
bool dm_ai_permission_allowed(dm_ai_permission_t p){(void)p;return allowed;}
static int dm_wifi_clock_valid(void){return 1;}
static int med_today(void){return 123;}
static int med_save(void){saves++;return save_rc;}
static void med_paint(void){}
static void dm_health_refresh(void){}
''' + function(text,"dm_med_ai_delete") + r'''
int main(void){dm_ai_delete_request_t r={.kind=DM_AI_DATA_MEDICATION,.selector=DM_AI_SELECT_TITLE};
strcpy(r.title,"维生素");strcpy(s_med[0].name,"维生素");strcpy(s_med[1].name,"胃药");
s_med[0].taken=3;s_med[0].times=2;s_med[0].hours[0]=8;s_med[0].hours[1]=18;
s_med[0].day=123;s_med[1].taken=1;s_med[1].day=123;
allowed=false;assert(dm_med_ai_delete(&r)==-2 && !saves);allowed=true;
save_rc=-1;assert(dm_med_ai_delete(&r)==-3 && s_med[0].taken==3);save_rc=0;
assert(!dm_med_ai_delete(&r) && s_med[0].taken==0 && s_med_n==2 && s_med[1].taken==1);
assert(!strcmp(s_med[0].name,"维生素") && s_med[0].times==2 && s_med[0].hours[1]==18);
assert(dm_med_ai_delete(&r)==-5);
s_med[0].taken=1;strcpy(s_med[1].name,"维生素");assert(dm_med_ai_delete(&r)==-6);
return 0;}
''')

    def test_voice_routing_and_negative_commands(self):
        cases = {
            "设定一个20分钟后的闹钟": "INTENT_ALARM_AFTER",
            "帮我设定一个20分钟后的闹钟": "INTENT_ALARM_AFTER",
            "我要玩2048": "INTENT_GAME",
            "帮我记一下买牛奶": "INTENT_NOTE",
            "我今天很开心": "INTENT_MOOD_RECORD",
            "记录心情，我很开心": "INTENT_MOOD_RECORD",
            "我想和你聊天": "INTENT_NONE",
            "我吃了苹果": "INTENT_NONE",
            "我刚吃了布洛芬": "INTENT_MED_TAKE",
            "我刚吃了阿莫西林": "INTENT_MED_TAKE",
            "吃药": "INTENT_MED_OPEN",
            "不要设置20分钟后的闹钟": "INTENT_CANCEL",
            "取消20分钟后的闹钟": "INTENT_CANCEL",
            "不要记录心情": "INTENT_CANCEL",
            "我还没吃药": "INTENT_CANCEL",
            "我吃药了吗": "INTENT_NONE",
            "删除我今天开心的心情记录": "INTENT_DELETE_UNSUPPORTED",
            "帮我清空健康数据": "INTENT_DELETE_UNSUPPORTED",
            "删除今天记录的服药信息": "INTENT_DELETE_UNSUPPORTED",
            "帮我记一下明天删除文件": "INTENT_NOTE",
        }
        main = "int main(void){assistant_intent_t item;\n"
        for text, expected in cases.items():
            main += ('parse_intent("' + text + '",&item);'
                     'assert(item.type == ' + expected + ');\n')
        main += ('parse_intent("设定一个20分钟后的闹钟",&item);'
                 'assert(item.seconds == 1200); return 0;}')
        self.run_c(self.parser() + main)

    def test_timed_meetings_take_precedence_over_generic_notes(self):
        cases = [
            ("30分钟之后有个会，帮我记一下", "开会"),
            ("30分钟之后有个会", "开会"),
            ("三十分钟以后有一个会，帮我记一下。", "开会"),
            ("帮我记一下30分钟后开会", "开会"),
            ("30分钟后提醒我开会，帮我记一下", "开会"),
            ("30分钟之后有个产品评审会议，帮我记一下", "产品评审会议"),
            ("帮我记一下30分钟后取快递", "取快递"),
        ]
        main = "int main(void){assistant_intent_t item;\n"
        for text, title in cases:
            main += ('assert(parse_intent("' + text + '",&item));'
                     'assert(item.type==INTENT_TODO_AFTER);'
                     'assert(item.seconds==1800);'
                     'assert(strcmp(item.text,"' + title + '")==0);\n')
        main += 'return 0;}'
        self.run_c(self.parser() + main)

    def test_meeting_routing_preserves_notes_questions_and_cancellation(self):
        cases = {
            "帮我记一下买牛奶": "INTENT_NOTE",
            "帮我记一下会议要点": "INTENT_NOTE",
            "帮我记一下写入备忘里：30分钟后开会": "INTENT_NOTE",
            "30分钟之后的会议怎么准备": "INTENT_NONE",
            "30分钟之后会下雨": "INTENT_NONE",
            "30分钟后不要提醒我开会": "INTENT_CANCEL",
            "帮我设一个30分钟后的闹钟": "INTENT_ALARM_AFTER",
            "帮我记一下明天删除文件": "INTENT_NOTE",
        }
        main = "int main(void){assistant_intent_t item;\n"
        for text, expected in cases.items():
            main += ('parse_intent("' + text + '",&item);'
                     'assert(item.type==' + expected + ');\n')
        main += 'return 0;}'
        self.run_c(self.parser() + main)

    def activity_log(self):
        text = source("dm_ai_log.c").replace("#include <nuttx/config.h>", "")
        text = text.replace('"/data/flowdesk_ai_activity.txt"', '"activity.txt"')
        text = text.replace('mkdir("/data", 0755)', 'mkdir(".", 0755)')
        return r'''
#define CONFIG_DESKMATE_APP 1
#include "dm_ai_log.h"
static bool clock_ready=true,fail_log_save;
int dm_wifi_clock_valid(void){return clock_ready;}
const char *dm_ai_permission_name(dm_ai_permission_t p)
{return p==DM_AI_TODO ? "待办事项" : p==DM_AI_READ_HEALTH ? "读取健康记录" : "permission";}
static int log_test_rename(const char *a,const char *b)
{if(fail_log_save){errno=EIO;return -1;}return rename(a,b);}
#define rename log_test_rename
''' + text

    def test_ai_activity_rotation_reload_and_detailed_denial(self):
        self.run_c(self.activity_log() + r'''
int main(void){dm_ai_log_entry_t rows[DM_AI_LOG_MAX];bool saved;uint32_t version;
char detail[1536];size_t count;int i;dm_ai_log_init();
assert(dm_ai_log_snapshot(rows,DM_AI_LOG_MAX,&version,&saved)==0 && saved);
for(i=0;i<55;i++)dm_ai_log_add(DM_AI_LOG_TODO,DM_AI_LOG_SUCCESS,
DM_AI_LOG_REASON_NONE,DM_AI_PERMISSION_COUNT,1800,0);
count=dm_ai_log_snapshot(rows,DM_AI_LOG_MAX,&version,&saved);
assert(count==50 && saved && rows[0].sequence==55 && rows[49].sequence==6);
dm_ai_log_init();assert(dm_ai_log_snapshot(rows,DM_AI_LOG_MAX,0,&saved)==50);
assert(rows[0].sequence==55 && rows[0].seconds==1800 && saved);
clock_ready=false;
dm_ai_log_add(DM_AI_LOG_TODO,DM_AI_LOG_DENIED,DM_AI_LOG_REASON_PERMISSION,
DM_AI_TODO,1800,-2);
dm_ai_log_snapshot(rows,DM_AI_LOG_MAX,0,&saved);
assert(!rows[0].clock_valid && rows[0].timestamp==0 && saved);
dm_ai_log_detail(&rows[0],detail,sizeof(detail));
assert(strstr(detail,"待办列表") && strstr(detail,"权限检查拦截"));
assert(strstr(detail,"待办事项") && strstr(detail,"30分00秒"));
assert(strstr(detail,"时间未校准") && !strstr(detail,"12345678"));
dm_ai_log_init();dm_ai_log_snapshot(rows,DM_AI_LOG_MAX,0,&saved);
assert(rows[0].result==DM_AI_LOG_DENIED && rows[0].permission==DM_AI_TODO);
return 0;}
''')

    def test_ai_activity_save_failure_preserves_disk_and_visible_result(self):
        self.run_c(self.activity_log() + r'''
int main(void){dm_ai_log_entry_t rows[DM_AI_LOG_MAX];bool saved;
dm_ai_log_init();dm_ai_log_add(DM_AI_LOG_NOTE,DM_AI_LOG_SUCCESS,
DM_AI_LOG_REASON_NONE,DM_AI_PERMISSION_COUNT,0,0);
fail_log_save=true;
dm_ai_log_add(DM_AI_LOG_TODO,DM_AI_LOG_FAILED,DM_AI_LOG_REASON_SAVE,
DM_AI_PERMISSION_COUNT,1800,-3);
assert(dm_ai_log_snapshot(rows,DM_AI_LOG_MAX,0,&saved)==2 && !saved);
assert(rows[0].action==DM_AI_LOG_TODO && rows[0].operation_rc==-3);
FILE*f=fopen("activity.txt","r");char line[256];int lines=0;
while(fgets(line,sizeof(line),f))lines++;fclose(f);assert(lines==2);
fail_log_save=false;
dm_ai_log_add(DM_AI_LOG_READ_HEALTH,DM_AI_LOG_DENIED,DM_AI_LOG_REASON_PERMISSION,
DM_AI_READ_HEALTH,0,-2);
dm_ai_log_init();assert(dm_ai_log_snapshot(rows,DM_AI_LOG_MAX,0,&saved)==3 && saved);
assert(rows[1].action==DM_AI_LOG_TODO && rows[1].reason==DM_AI_LOG_REASON_SAVE);
assert(access("activity.txt.tmp",F_OK)!=0);
return 0;}
''')

    def test_ai_activity_rejects_malformed_entries(self):
        self.run_c(self.activity_log() + r'''
int main(void){dm_ai_log_entry_t rows[DM_AI_LOG_MAX];bool saved;
FILE*f=fopen("activity.txt","w");assert(f);
fputs(LOG_VERSION,f);fprintf(f,"1 0 0 %d %d %d %d 0 0\n",
DM_AI_LOG_NOTE,DM_AI_LOG_SUCCESS,DM_AI_LOG_REASON_NONE,DM_AI_PERMISSION_COUNT);
fputs("2 0 0 999999 0 0 0 0 0\n",f);fclose(f);
dm_ai_log_init();assert(dm_ai_log_snapshot(rows,DM_AI_LOG_MAX,0,&saved)==1 && !saved);
dm_ai_log_add(DM_AI_LOG_NOTE,DM_AI_LOG_SUCCESS,DM_AI_LOG_REASON_NONE,
DM_AI_PERMISSION_COUNT,0,0);
dm_ai_log_init();assert(dm_ai_log_snapshot(rows,DM_AI_LOG_MAX,0,&saved)==2 && saved);
return 0;}
''')

    def test_ai_activity_serializes_concurrent_writers(self):
        self.run_c(self.activity_log() + r'''
static void*log_thread(void*p){(void)p;for(int i=0;i<20;i++)
dm_ai_log_add(DM_AI_LOG_NOTE,DM_AI_LOG_SUCCESS,DM_AI_LOG_REASON_NONE,
DM_AI_PERMISSION_COUNT,0,0);return NULL;}
int main(void){pthread_t a,b,c;dm_ai_log_entry_t rows[DM_AI_LOG_MAX];bool saved;
dm_ai_log_init();pthread_create(&a,0,log_thread,0);pthread_create(&b,0,log_thread,0);
pthread_create(&c,0,log_thread,0);pthread_join(a,0);pthread_join(b,0);pthread_join(c,0);
assert(dm_ai_log_snapshot(rows,DM_AI_LOG_MAX,0,&saved)==50 && saved);
assert(rows[0].sequence==60 && rows[49].sequence==11);
dm_ai_log_init();assert(dm_ai_log_snapshot(rows,DM_AI_LOG_MAX,0,&saved)==50 && saved);
for(int i=0;i<49;i++)assert(rows[i].sequence==rows[i+1].sequence+1);
return 0;}
''')

    def test_local_life_reads_share_one_gate_and_recheck_revocation(self):
        implementation = source("dm_ai_local_read.c").replace("#include <nuttx/config.h>", "")
        self.run_c(r'''
#define CONFIG_DESKMATE_APP 1
#include "dm_ai_permissions.h"
#include "dm_ai_local_read.h"
static bool allowed,revoke_during_snapshot;static int calls;
bool dm_ai_permission_allowed(dm_ai_permission_t p)
{return p==DM_AI_READ_LOCAL_DATA && allowed;}
static int records(dm_ai_local_record_t*out,int cap)
{assert(cap>=2);calls++;strcpy(out[0].title,"LOCAL_RECORD_SECRET");
strcpy(out[1].title,"second");if(revoke_during_snapshot)allowed=false;return 2;}
int dm_note_ai_snapshot(dm_ai_local_record_t*out,int cap){return records(out,cap);}
int dm_reminder_ai_snapshot(bool alarm,dm_ai_local_record_t*out,int cap)
{(void)alarm;return records(out,cap);}
int dm_countdown_ai_snapshot(dm_ai_local_record_t*out,int cap){return records(out,cap);}
''' + implementation + r'''
int main(void){char text[512];int i;
for(i=0;i<DM_AI_READ_KIND_COUNT;i++){
strcpy(text,"old");assert(dm_ai_read_local(i,text,sizeof(text))==-2);
assert(!text[0] && calls==0);}
allowed=true;
for(i=0;i<DM_AI_READ_KIND_COUNT;i++){
assert(dm_ai_read_local(i,text,sizeof(text))==0);
assert(strstr(text,"LOCAL_RECORD_SECRET"));}
assert(calls==4);revoke_during_snapshot=true;
assert(dm_ai_read_local(DM_AI_READ_NOTES,text,sizeof(text))==-2);
assert(!text[0] && calls==5);return 0;}
''')

    def test_local_read_queries_never_become_writes(self):
        self.run_c(self.parser() + r'''
int main(void){assistant_intent_t i;
assert(parse_intent("读一下备忘",&i) && i.type==INTENT_READ_LOCAL && i.read_kind==DM_AI_READ_NOTES);
assert(parse_intent("我有哪些待办？",&i) && i.type==INTENT_READ_LOCAL && i.read_kind==DM_AI_READ_TODOS);
assert(parse_intent("查看闹钟列表",&i) && i.type==INTENT_READ_LOCAL && i.read_kind==DM_AI_READ_ALARMS);
assert(parse_intent("查询倒数日",&i) && i.type==INTENT_READ_LOCAL && i.read_kind==DM_AI_READ_COUNTDOWNS);
assert(parse_intent("不要读取备忘",&i) && i.type==INTENT_CANCEL);
assert(parse_intent("删除备忘内容",&i) && i.type==INTENT_DELETE_DATA);
assert(parse_intent("帮我记一下wifi密码是12345678",&i) && i.type==INTENT_NOTE);
return 0;}
''')

    def test_local_record_providers_check_permission_before_copying(self):
        self.run_c(r'''
#include "dm_ai_permissions.h"
#include "dm_ai_local_read.h"
static bool reading_allowed;
bool dm_ai_permission_allowed(dm_ai_permission_t p)
{return p==DM_AI_READ_LOCAL_DATA && reading_allowed;}
static char s_notes[8][32]={"NOTE_SECRET"};static int s_note_n=1;
#define REMINDER_TODO 0
#define REMINDER_ALARM 1
static struct {int type,fired,pending,repeat_daily;time_t due;char text[64];} s_items[4];
static int s_count=4;
static struct {int y,m,d,hour,minute,has_alarm;char name[48];} s_cd[8];
static int s_cd_n=1;
''' + function(source("dm_ui_note.c"), "dm_note_ai_snapshot") + "\n" +
            function(source("dm_ui_remind.c"), "dm_reminder_ai_snapshot") + "\n" +
            function(source("dm_ui_life.c"), "dm_countdown_ai_snapshot") + r'''
int main(void){dm_ai_local_record_t rows[12],before[12];memset(rows,0xa5,sizeof(rows));
memcpy(before,rows,sizeof(rows));
assert(dm_note_ai_snapshot(rows,12)==-2);
assert(dm_reminder_ai_snapshot(false,rows,12)==-2);
assert(dm_countdown_ai_snapshot(rows,12)==-2);
assert(memcmp(rows,before,sizeof(rows))==0);
reading_allowed=true;assert(dm_note_ai_snapshot(rows,12)==1);
assert(strcmp(rows[0].title,"NOTE_SECRET")==0);
s_items[0].type=REMINDER_TODO;strcpy(s_items[0].text,"active");
s_items[1].type=REMINDER_TODO;s_items[1].fired=1;strcpy(s_items[1].text,"completed");
s_items[2].type=REMINDER_TODO;s_items[2].fired=1;s_items[2].pending=1;strcpy(s_items[2].text,"pending");
s_items[3].type=REMINDER_ALARM;strcpy(s_items[3].text,"alarm");
assert(dm_reminder_ai_snapshot(false,rows,12)==2 && strcmp(rows[1].title,"pending")==0);
assert(dm_reminder_ai_snapshot(true,rows,12)==1 && strcmp(rows[0].title,"alarm")==0);
s_cd[0].y=2027;s_cd[0].m=1;s_cd[0].d=2;strcpy(s_cd[0].name,"event");
assert(dm_countdown_ai_snapshot(rows,12)==1 && rows[0].year==2027);
return 0;}
''')

    def test_revoked_life_read_is_suppressed_before_worker_receives_it(self):
        text = source("dm_assistant.c")
        self.run_c(self.parser() + r'''
static bool allowed=true;static char answer[512];
bool dm_ai_permission_allowed(dm_ai_permission_t p){(void)p;return allowed;}
static int run_intent(assistant_intent_t*i,char*out,size_t cap)
{(void)i;snprintf(out,cap,"LOCAL_LIFE_SECRET");allowed=false;return 0;}
''' + function(text,"dm_assistant_intent_try") + "\n" +
            function(text,"dm_assistant_intent_tick") + r'''
static void*request(void*p){(void)p;
assert(dm_assistant_intent_try("读一下备忘",answer,sizeof(answer))==4);return NULL;}
int main(void){pthread_t th;int active;pthread_create(&th,0,request,0);
do{pthread_mutex_lock(&s_intent_lock);active=s_intent_active;pthread_mutex_unlock(&s_intent_lock);}while(!active);
dm_assistant_intent_tick();pthread_join(th,0);
assert(strstr(answer,"关闭") && !strstr(answer,"LOCAL_LIFE_SECRET"));return 0;}
''')

    def test_revoked_health_query_is_suppressed_before_worker_receives_it(self):
        text = source("dm_assistant.c")
        self.run_c(self.parser() + r'''
static bool allowed=true;static char answer[512];
bool dm_ai_permission_allowed(dm_ai_permission_t p){(void)p;return allowed;}
static int run_intent(assistant_intent_t*i,char*out,size_t cap)
{(void)i;snprintf(out,cap,"LOCAL_HEALTH_SECRET");allowed=false;return 0;}
''' + function(text,"dm_assistant_intent_try") + "\n" +
            function(text,"dm_assistant_intent_tick") + r'''
static void*request(void*p){(void)p;
assert(dm_assistant_intent_try("最近一条心情是什么时候",answer,sizeof(answer))==5);return NULL;}
int main(void){pthread_t th;int active;pthread_create(&th,0,request,0);
do{pthread_mutex_lock(&s_intent_lock);active=s_intent_active;pthread_mutex_unlock(&s_intent_lock);}while(!active);
dm_assistant_intent_tick();pthread_join(th,0);
assert(strstr(answer,"关闭") && !strstr(answer,"LOCAL_HEALTH_SECRET"));return 0;}
''')

    def test_health_http_body_is_not_sent_when_permission_changes_during_wait(self):
        text = source("dm_voice.c")
        self.run_c(r'''
#include "dm_ai_permissions.h"
#include "dm_voice.h"
#define JSON_REQ "request.json"
#define VOICE_BASE "mock://api"
#define VOICE_WIFI_WAIT_SEC 10
static dm_voice_failure_t s_last_failure;
static bool allowed=true,revoke_during_wait;static int uploads,permission_checks,revoke_on_check;
bool dm_ai_permission_allowed(dm_ai_permission_t p)
{(void)p;if(++permission_checks==revoke_on_check)allowed=false;return allowed;}
static int dm_wifi_connected(void){return 1;}
static int voice_wait_for_wifi(void){return 0;}
static int dm_wifi_wait_time_sync(int t){(void)t;if(revoke_during_wait)allowed=false;return 0;}
static int run_curl_command(const char*u,const char*p,const char*r,int t)
{(void)u;(void)p;(void)r;(void)t;uploads++;return 0;}
''' + function(text,"http_post_json_checked") + "\n" +
                   function(text,"http_post_json") + r'''
int main(void){revoke_during_wait=true;
assert(http_post_json_checked("/tts","LOCAL_HEALTH_SECRET","response.json",true)==-2);
assert(!uploads && access(JSON_REQ,F_OK)!=0);
/* A revocation after body creation must still block the network call. */
revoke_during_wait=false;allowed=true;permission_checks=0;revoke_on_check=2;
assert(http_post_json_checked("/tts","LOCAL_HEALTH_SECRET","response.json",true)==-2);
assert(!uploads && access(JSON_REQ,F_OK)!=0);
/* User-supplied audio and ordinary chat remain usable with health off. */
assert(!http_post_json("/asr","USER_PROVIDED_AUDIO","response.json") && uploads==1);
allowed=true;revoke_on_check=0;assert(!http_post_json_checked("/tts","ALLOWED_HEALTH","response.json",true));
assert(uploads==2);return 0;}
''')

    def test_life_read_result_is_hidden_after_permission_is_switched_off(self):
        self.run_c(r'''
#include "dm_ai_permissions.h"
#define FACE_HAPPY 1
#define EYE_DECOR_NONE 0
static bool allowed,s_last_ans_local,s_out_local,s_ui_rec,s_busy,s_out_health,s_last_ans_health;
static int s_out_ready,s_rec_sec,s_mic_err;static uint64_t s_rec_started_ms;
static char s_out_ans[224],s_out_sub[80],shown[512];
bool dm_ai_permission_allowed(dm_ai_permission_t p){(void)p;return allowed;}
static const char*dm_t(const char*z,const char*e){(void)e;return z;}
static void set_ans(const char*s){snprintf(shown,sizeof(shown),"%s",s);}
static void set_sub(const char*s){(void)s;}
static void dm_face_set(int a,int b,int c){(void)a;(void)b;(void)c;}
static void reset_to_idle(void){}
static uint64_t chat_monotonic_ms(void){return 0;}
static int dm_voice_rec_active(void){return 0;}
static void kick_job(int j){(void)j;}
''' + function(source("dm_ui_chat.c"),"dm_chat_tick") + r'''
int main(void){s_out_local=true;s_out_ready=1;strcpy(s_out_ans,"LOCAL_SECRET");
dm_chat_tick();assert(!strstr(shown,"LOCAL_SECRET") && !strstr(s_out_ans,"LOCAL_SECRET"));
allowed=true;s_out_ready=1;strcpy(s_out_ans,"LOCAL_SECRET");
dm_chat_tick();assert(strstr(shown,"LOCAL_SECRET"));
allowed=false;dm_chat_tick();assert(!strstr(shown,"LOCAL_SECRET"));
s_out_local=false;s_out_health=true;s_out_ready=1;strcpy(s_out_ans,"HEALTH_SECRET");
dm_chat_tick();assert(!strstr(shown,"HEALTH_SECRET") && !strstr(s_out_ans,"HEALTH_SECRET"));
allowed=true;s_out_ready=1;strcpy(s_out_ans,"HEALTH_SECRET");dm_chat_tick();
assert(strstr(shown,"HEALTH_SECRET"));allowed=false;dm_chat_tick();
assert(!strstr(shown,"HEALTH_SECRET"));return 0;}
''')

    def test_old_log_permission_sentinel_does_not_become_a_denial(self):
        self.run_c(self.activity_log() + r'''
int main(void){dm_ai_log_entry_t rows[2];bool saved;char text[1536];
FILE*f=fopen("activity.txt","w");fputs(LOG_VERSION,f);
fprintf(f,"1 0 0 %d %d %d 15 0 0\n",DM_AI_LOG_NOTE,DM_AI_LOG_SUCCESS,DM_AI_LOG_REASON_NONE);
fclose(f);dm_ai_log_init();assert(dm_ai_log_snapshot(rows,2,0,&saved)==1 && saved);
assert(rows[0].permission==DM_AI_PERMISSION_COUNT);
dm_ai_log_detail(&rows[0],text,sizeof(text));assert(!strstr(text,"被关闭权限"));
dm_ai_log_add(DM_AI_LOG_READ_NOTES,DM_AI_LOG_DENIED,DM_AI_LOG_REASON_PERMISSION,
DM_AI_READ_LOCAL_DATA,0,-2);dm_ai_log_init();dm_ai_log_snapshot(rows,2,0,&saved);
assert(saved && rows[0].permission==DM_AI_READ_LOCAL_DATA);
return 0;}
''')

    def test_permission_explanations_and_old_denials_survive_new_read_bit(self):
        implementation = source("dm_ai_permissions.c").replace("#include <nuttx/config.h>", "")
        implementation = implementation.replace('"/data/flowdesk_ai_permissions.txt"', '"permissions.txt"')
        implementation = implementation.replace('mkdir("/data", 0755)', 'mkdir(".", 0755)')
        self.run_c('#define CONFIG_DESKMATE_APP 1\n' + implementation + r'''
int main(void){assert(DM_AI_READ_HEALTH==14 && DM_AI_READ_LOCAL_DATA==15);
FILE*f=fopen("permissions.txt","w");
fprintf(f,"FLOWDESK_AI_PERMISSIONS_V1 %u\n",1U<<DM_AI_NOTE);fclose(f);
dm_ai_permissions_init();assert(!dm_ai_permission_allowed(DM_AI_NOTE));
assert(dm_ai_permission_allowed(DM_AI_READ_LOCAL_DATA));
assert(dm_ai_permission_set(DM_AI_READ_LOCAL_DATA,false)==0);
assert(dm_ai_permission_allowed(DM_AI_READ_HEALTH));
assert(dm_ai_permission_set(DM_AI_READ_HEALTH,false)==0);
assert(dm_ai_permission_set(DM_AI_READ_LOCAL_DATA,true)==0);
assert(dm_ai_permission_allowed(DM_AI_READ_LOCAL_DATA) && !dm_ai_permission_allowed(DM_AI_READ_HEALTH));
dm_ai_permissions_init();assert(dm_ai_permission_allowed(DM_AI_READ_LOCAL_DATA));
assert(!dm_ai_permission_allowed(DM_AI_NOTE));
for(int i=0;i<DM_AI_PERMISSION_COUNT;i++)assert(strlen(dm_ai_permission_explanation(i))>100);
const char*s=dm_ai_permission_explanation(DM_AI_READ_LOCAL_DATA);
assert(strstr(s,"备忘")&&strstr(s,"待办")&&strstr(s,"闹钟")&&strstr(s,"倒数日"));
return 0;}
''')

    def medicine(self):
        text = source("dm_ui_med.c")
        part = text[text.index("static int med_voice_has_any("):
                    text.index("void dm_create_med(")]
        return r'''
typedef struct {char name[32]; uint8_t times,hours[3],taken;uint16_t day;} med_item_t;
static med_item_t s_med[8];
static int s_med_n=1,s_med_sel,s_med_loaded=1,save_rc;
static void *s_med_tip;
static int med_today(void){return 123;}
static int med_save(void){return save_rc;}
static void med_load(void){}
static void med_paint(void){}
static int dm_wifi_clock_valid(void){return 1;}
static void lv_label_set_text(void *obj,const char *s){(void)obj;(void)s;}
''' + part

    def test_medicine_matching_slots_and_rollback(self):
        self.run_c(self.medicine() + r'''
int main(void){char reply[512];int rc;time_t now=time(NULL);struct tm current;
localtime_r(&now,&current);
strcpy(s_med[0].name,"维生素");s_med[0].times=3;s_med[0].day=123;
s_med[0].hours[0]=0;s_med[0].hours[1]=24;s_med[0].hours[2]=25;
rc=dm_med_voice_take("我刚吃了维生素",reply,sizeof(reply));
assert(rc==0 && s_med[0].taken==1);
rc=dm_med_voice_take("我刚吃了维生素",reply,sizeof(reply));
assert(rc!=0 && s_med[0].taken==1);
s_med[0].taken=0;
rc=dm_med_voice_take("我吃了药，布洛芬",reply,sizeof(reply));
assert(rc!=0 && s_med[0].taken==0);
rc=dm_med_voice_take("我吃了晚饭，然后吃了维生素",reply,sizeof(reply));
assert(rc==0 && s_med[0].taken==1);
s_med[0].taken=0;save_rc=-1;
rc=dm_med_voice_take("我刚吃了维生素",reply,sizeof(reply));
assert(rc==-5 && s_med[0].taken==0);
save_rc=0;
rc=dm_med_voice_take("我没有吃维生素",reply,sizeof(reply));
assert(rc!=0 && s_med[0].taken==0);
return 0;}
''')

    def test_running_intent_cannot_be_reused_after_timeout(self):
        text = source("dm_assistant.c")
        parser = self.parser().replace("#define INTENT_WAIT_STEPS 160",
                                       "#define INTENT_WAIT_STEPS 2")
        parser = parser.replace("#define INTENT_WAIT_US 50000",
                                "#define INTENT_WAIT_US 1000")
        self.run_c('#include "dm_ai_permissions.h"\n' + parser + r'''
bool dm_ai_permission_allowed(dm_ai_permission_t p){(void)p;return true;}
static int run_intent(const assistant_intent_t *intent,char *reply,size_t cap)
{(void)intent;usleep(100000);snprintf(reply,cap,"completed-A");return 0;}
''' + function(text, "dm_assistant_intent_try") + "\n" +
            function(text, "dm_assistant_intent_tick") + r'''
static char answer[256];
static void *request(void *p){(void)p;assert(dm_assistant_intent_try("计时20分钟",answer,sizeof(answer))>0);return NULL;}
static void *tick(void *p){(void)p;dm_assistant_intent_tick();return NULL;}
int main(void){pthread_t a,b;char other[256];int state;
pthread_create(&a,NULL,request,NULL);
do {pthread_mutex_lock(&s_intent_lock);state=s_intent_active;pthread_mutex_unlock(&s_intent_lock);}while(!state);
pthread_create(&b,NULL,tick,NULL);
do {pthread_mutex_lock(&s_intent_lock);state=s_intent_executing;pthread_mutex_unlock(&s_intent_lock);}while(!state);
usleep(20000);
assert(dm_assistant_intent_try("计时30分钟",other,sizeof(other))==1);
assert(strstr(other,"仍在处理"));
pthread_join(a,NULL);pthread_join(b,NULL);assert(strcmp(answer,"completed-A")==0);
assert(!s_intent_active);assert(audit_calls==2 && audit_action==DM_AI_LOG_FOCUS);
assert(audit_result==DM_AI_LOG_SUCCESS && audit_seconds==1200);return 0;}
''')

    def test_note_save_failure_preserves_original_file(self):
        text = source("dm_ui_note.c")
        self.run_c(r'''
#define NOTE_PATH "notes.txt"
#define NOTE_MAX 8
#define NOTE_LEN_MAX 32
static char s_notes[NOTE_MAX][NOTE_LEN_MAX];static int s_note_n=1;
static int fail_rename(const char *a,const char *b){(void)a;(void)b;return -1;}
#define rename fail_rename
''' + function(text, "note_save") + r'''
int main(void){char buf[32];FILE *f=fopen(NOTE_PATH,"w");fputs("original",f);fclose(f);
strcpy(s_notes[0],"replacement");assert(note_save()!=0);
f=fopen(NOTE_PATH,"r");assert(f);assert(fgets(buf,sizeof(buf),f));fclose(f);
assert(strcmp(buf,"original")==0);return 0;}
''')

    def reminders(self):
        text = source("dm_ui_remind.c")
        declarations = text[text.index("#define REMINDER_PATH"):
                            text.index("static lv_obj_t *s_todo_rows")]
        declarations = declarations.replace('"/data/flowdesk_reminders.txt"',
                                            '"reminders.txt"')
        return declarations

    def test_reminder_legacy_and_pending_queue_persistence(self):
        text = source("dm_ui_remind.c")
        self.run_c(self.reminders() + function(text, "reminder_save") + "\n" +
                   function(text, "reminder_load") + r'''
int main(void){FILE *f=fopen(REMINDER_PATH,"w");
fputs("0 0 0 1800000000 0 0 old task\n",f);fclose(f);
reminder_load();assert(s_count==1 && !s_items[0].pending);
s_items[0].fired=1;s_items[0].pending=1;assert(reminder_save()==0);
memset(s_items,0,sizeof(s_items));s_count=0;reminder_load();
assert(s_count==1 && s_items[0].pending && s_items[0].fired);
assert(strcmp(s_items[0].text,"old task")==0);return 0;}
''')

    def test_reminders_scan_due_items_despite_save_failure(self):
        text = source("dm_ui_remind.c")
        self.run_c(self.reminders() + r'''
static void *s_alert,*s_alert_title,*s_alert_text;
#define LV_OBJ_FLAG_HIDDEN 1
static void lv_label_set_text(void *p,const char *s){(void)p;(void)s;}
static void lv_obj_clear_flag(void *p,int f){(void)p;(void)f;}
static void lv_obj_move_foreground(void *p){(void)p;}
static void reminder_paint(void){}
static int reminder_save(void){return -1;}
static int reminder_clock_valid(time_t t){(void)t;return 1;}
static int reminder_speak(int i){(void)i;return 0;}
static void reminder_alert_dismiss(void *p){(void)p;s_alert_idx=-1;s_alert_sound_started=0;}
''' + function(text, "dm_reminder_tick") + r'''
int main(void){time_t now=time(NULL);s_count=2;
s_items[0].type=REMINDER_TODO;s_items[0].due=now-1;
s_items[1].type=REMINDER_ALARM;s_items[1].due=now-1;
dm_reminder_tick();assert(s_alert_idx==1);assert(s_items[0].pending);
assert(s_items[0].fired && s_items[1].fired && s_persistence_dirty);
return 0;}
''')

    def test_focus_counts_real_elapsed_time(self):
        text = source("dm_ui_focus.c")
        body = function(text, "dm_focus_run_tick")
        body = body[:body.index("  if (g_dm.page == PAGE_FOCUS_RUN)")] + "}\n"
        self.run_c(r'''
static uint64_t s_focus_last_ms,s_focus_partial_ms,now_ms;
static struct {int focus_run,focus_finished;int32_t focus_left;} g_dm;
static uint64_t focus_now_ms(void){return now_ms;}
''' + body + r'''
int main(void){g_dm.focus_run=1;g_dm.focus_left=1200;s_focus_last_ms=1000;
now_ms=1250;dm_focus_run_tick();assert(g_dm.focus_left==1200);
now_ms=4750;dm_focus_run_tick();assert(g_dm.focus_left==1197);
now_ms=5000;dm_focus_run_tick();assert(g_dm.focus_left==1196);
g_dm.focus_run=0;now_ms=10000;dm_focus_run_tick();assert(g_dm.focus_left==1196);
g_dm.focus_run=1;now_ms=11000;dm_focus_run_tick();assert(g_dm.focus_left==1195);
return 0;}
''')

    def test_proactive_medication_uses_saved_date_encoding(self):
        text = source("dm_proactive.c")
        part = function(text, "med_due_count").replace(
            '"/data/deskmate_med.txt"', '"meds.txt"')
        self.run_c(part + r'''
int main(void){time_t now=time(NULL);struct tm current;FILE *f;
localtime_r(&now,&current);int day=(current.tm_yday+current.tm_year*1000)&0xffff;
f=fopen("meds.txt","w");fprintf(f,"vitamin|1|%d,0,0|1|%d\n",current.tm_hour,day);fclose(f);
assert(med_due_count()==0);return 0;}
''')

    def test_wifi_empty_or_failed_status_is_not_connected(self):
        text = source("dm_wifi.c")
        part = function(text, "wifi_is_link_up").replace(
            '"/tmp/wifi_status.txt"', '"wifi-status.txt"')
        self.run_c(r'''
static int command_rc;
static int fake_system(const char *s){(void)s;return command_rc;}
#define system fake_system
''' + part + r'''
int main(void){FILE *f=fopen("wifi-status.txt","w");fclose(f);
assert(wifi_is_link_up()==0);
f=fopen("wifi-status.txt","w");fputs("AP: 11:22:33:44:55:66\n",f);fclose(f);
assert(wifi_is_link_up()==1);command_rc=1;assert(wifi_is_link_up()==-1);
command_rc=0;f=fopen("wifi-status.txt","w");fputs("AP: 00:00:00:00:00:00\n",f);fclose(f);
assert(wifi_is_link_up()==0);return 0;}
''')

    def test_seed_clock_requires_manual_confirmation_or_ntp(self):
        text = source("dm_wifi.c")
        self.run_c(r'''
static int s_clock_seed_only=1,s_manual_time_set,samples,status_calls;
static pthread_mutex_t s_ntp_lock=PTHREAD_MUTEX_INITIALIZER;
static int wifi_ntp_has_samples(void){status_calls++;return samples;}
''' + function(text, "wifi_clock_in_range") + "\n" +
                   function(text, "wifi_clock_synced") + "\n" +
                   function(text, "dm_wifi_clock_valid") + r'''
int main(void){assert(!dm_wifi_clock_valid());samples=1;
assert(dm_wifi_clock_valid());assert(!s_clock_seed_only);
samples=0;status_calls=0;s_manual_time_set=1;
assert(dm_wifi_clock_valid());assert(status_calls==0);
assert(wifi_clock_synced());assert(status_calls==0);
assert(!wifi_clock_in_range(0));return 0;}
''')

    def test_manual_time_is_verified_and_readable_by_voice_thread(self):
        text = source("dm_wifi.c")
        self.run_c(r'''
#include <sys/time.h>
#define DM_WIFI_MANUAL_TIME_PATH "manual-time.txt"
static int s_clock_seed_only=1,s_manual_time_set,samples,status_calls;
static int set_rc,readback_bad;
static time_t fake_now=1546300800;
static uint64_t s_ntp_retry_after_ms;
static pthread_mutex_t s_ntp_lock=PTHREAD_MUTEX_INITIALIZER;
#define DM_WIFI_NTP_RETRY_SEC 120
struct ntpc_status_s{unsigned nsamples;};
static int ntpc_status(struct ntpc_status_s *s){(void)s;assert(0);return 0;}
static int dm_wifi_connected(void){return 1;}
static void wifi_restart_ntp_after_link(void){assert(0);}
static int wifi_ntp_has_samples(void){status_calls++;return samples;}
static time_t fake_time(time_t *out){if(out)*out=fake_now;return fake_now;}
static int fake_settimeofday(const struct timeval *tv,const void *tz)
{(void)tz;if(set_rc){errno=EPERM;return -1;}
 fake_now=tv->tv_sec+(readback_bad?60:0);return 0;}
#define time fake_time
#define settimeofday fake_settimeofday
''' + function(text, "wifi_clock_in_range") + "\n" +
                   function(text, "wifi_clock_synced") + "\n" +
                   function(text, "dm_wifi_set_manual_time") + "\n" +
                   function(text, "dm_wifi_manual_time_restore") + "\n" +
                   function(text, "dm_wifi_wait_time_sync") + r'''
static void *voice_worker(void *unused)
{(void)unused;assert(dm_wifi_wait_time_sync(40)==0);return NULL;}
int main(void){pthread_t worker;const time_t epoch=1794056400;
assert(dm_wifi_set_manual_time(0)==-EINVAL);assert(!s_manual_time_set);
set_rc=1;assert(dm_wifi_set_manual_time(epoch)==-EPERM);
assert(!s_manual_time_set);
set_rc=0;readback_bad=1;assert(dm_wifi_set_manual_time(epoch)==-EIO);
assert(!s_manual_time_set);
readback_bad=0;assert(dm_wifi_set_manual_time(epoch)==0);
assert(s_manual_time_set && !s_clock_seed_only);
assert(!pthread_create(&worker,NULL,voice_worker,NULL));
pthread_join(worker,NULL);assert(status_calls==0);
/* Simulate a reboot without an RTC: the saved epoch is not the new time. */
fake_now=1546300800;s_manual_time_set=0;s_clock_seed_only=0;
dm_wifi_manual_time_restore();assert(fake_now==epoch);
assert(s_clock_seed_only && !s_manual_time_set && !wifi_clock_synced());
assert(dm_wifi_set_manual_time(epoch)==0);assert(wifi_clock_synced());
/* A missing /data equivalent cannot undo a successfully applied clock. */
assert(remove("manual-time.txt")==0);
assert(mkdir("manual-time.txt",0700)==0);
assert(dm_wifi_set_manual_time(epoch)==0);assert(wifi_clock_synced());
return 0;}
'''.replace('int main(void)', '#include <sys/stat.h>\nint main(void)'))

    def test_time_apply_ui_uses_verified_backend(self):
        self.run_c(r'''
typedef void lv_event_t;
static int s_time_value[5]={2026,11,7,21,0},calls,rc,paint_calls;
static void *s_time_hint_l=(void *)1;
static char hint[256];
static int time_days_in_month(int y,int m){(void)y;(void)m;return 30;}
static const char *dm_t(const char *zh,const char *en){(void)en;return zh;}
static void lv_label_set_text(void *l,const char *s)
{(void)l;snprintf(hint,sizeof(hint),"%s",s);}
static int dm_wifi_set_manual_time(time_t epoch){assert(epoch>0);calls++;return rc;}
static void time_refresh_labels(void){paint_calls++;}
''' + function(source("dm_ui_wifi.c"), "time_apply_cb") + r'''
int main(void){time_apply_cb(NULL);assert(calls==1 && paint_calls==1);
assert(strstr(hint,"本次开机已校准"));rc=-EPERM;time_apply_cb(NULL);
assert(calls==2 && paint_calls==1 && strstr(hint,"时间未生效"));
s_time_value[1]=13;time_apply_cb(NULL);assert(calls==2);return 0;}
''')

    def test_ntp_refresh_does_not_lock_out_manual_time_or_repeat_early(self):
        text = source("dm_wifi.c")
        self.run_c(r'''
#define DM_WIFI_NTP_SERVERS "test.invalid"
#define DM_WIFI_NTP_RETRY_SEC 120
static pthread_mutex_t s_ntp_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_ntp_retry_after_ms,now_ms=1000;
static int s_ntp_refresh_running,s_ntp_servers_selected;
static int samples,stop_calls,start_calls,stop_rc;
static int wifi_ntp_has_samples(void){return samples;}
static uint64_t wifi_monotonic_ms(void){return now_ms;}
static int ntpc_stop(void){stop_calls++;
 assert(pthread_mutex_trylock(&s_ntp_lock)==0);
 pthread_mutex_unlock(&s_ntp_lock);return stop_rc;}
static int ntpc_start_with_list(const char *s){(void)s;start_calls++;
 assert(pthread_mutex_trylock(&s_ntp_lock)==0);
 pthread_mutex_unlock(&s_ntp_lock);return 435;}
''' + function(text, "wifi_restart_ntp_after_link") + r'''
int main(void){wifi_restart_ntp_after_link();assert(stop_calls==1 && start_calls==1);
now_ms+=30000;wifi_restart_ntp_after_link();assert(stop_calls==1);
now_ms=121000;s_ntp_refresh_running=1;wifi_restart_ntp_after_link();
assert(stop_calls==1);s_ntp_refresh_running=0;stop_rc=-EIO;
wifi_restart_ntp_after_link();assert(stop_calls==1 && start_calls==2);
/* Initial configuration failure must not start another daemon. */
now_ms+=120000;s_ntp_servers_selected=0;
wifi_restart_ntp_after_link();assert(stop_calls==2 && start_calls==2);
now_ms+=120000;samples=1;wifi_restart_ntp_after_link();assert(stop_calls==2);
return 0;}
''')

    def test_time_wait_accepts_manual_calibration_at_deadline(self):
        text = source("dm_wifi.c")
        self.run_c(r'''
#define DM_WIFI_NTP_RETRY_SEC 120
static int checks,ready_at,connected=1;
static int s_manual_time_set,s_clock_seed_only;
static pthread_mutex_t s_ntp_lock=PTHREAD_MUTEX_INITIALIZER;
struct ntpc_status_s{unsigned nsamples;};
static int wifi_clock_synced(void){return ++checks>=ready_at;}
static int dm_wifi_connected(void){return connected;}
static void wifi_restart_ntp_after_link(void){}
static int ntpc_status(struct ntpc_status_s *s){s->nsamples=0;return 0;}
''' + function(text, "dm_wifi_wait_time_sync") + r'''
int main(void){ready_at=2;assert(dm_wifi_wait_time_sync(0)==0);
checks=0;ready_at=99;assert(dm_wifi_wait_time_sync(0)==-ETIMEDOUT);
checks=0;connected=0;assert(dm_wifi_wait_time_sync(0)==-ENETDOWN);
return 0;}
''')

    def test_sdk_ntp_samples_do_not_require_dns_ping(self):
        configured_sdk = os.environ.get("FLOWDESK_SDK")
        if not configured_sdk:
            self.skipTest("Set FLOWDESK_SDK to test the actual patched SDK")
        sdk = pathlib.Path(configured_sdk)
        path = sdk / "apps/netutils/ntpclient/ntpclient.c"
        if not path.is_file():
            self.skipTest("Set FLOWDESK_SDK to test the actual patched SDK")
        text = path.read_text(encoding="utf-8")
        start = text.index("      nsamples = 0;", text.index("static int ntpc_daemon("))
        end = text.index("      /* Analyse samples. */", start)
        self.run_c(r'''
#define CONFIG_NETUTILS_NTPCLIENT_NUM_SAMPLES 5
#define NTP_STOP_REQUESTED 9
static struct {int state;} g_ntpc_daemon;
static int calls,failures;
static int ntpc_get_ntp_sample(void *s,void *samples,int n)
{(void)s;(void)samples;(void)n;calls++;errno=ETIMEDOUT;
return calls<=failures?-1:0;}
static int collect(void){int nsamples,ret,errval=0,i;void *srvs=NULL,*samples=NULL;
''' + text[start:end] + r'''
(void)errval;return nsamples;}
int main(void){assert(collect()==5 && calls==5);
calls=0;failures=2;assert(collect()==3 && calls==5);
calls=0;g_ntpc_daemon.state=NTP_STOP_REQUESTED;
assert(collect()==0 && calls==0);return 0;}
''')

    def test_long_voice_note_is_rejected_without_truncation(self):
        self.run_c(r'''
#define NOTE_MAX 8
#define NOTE_LEN_MAX 32
#define PAGE_NOTE 1
static char s_notes[8][32];static int s_note_n,s_note_sel;
static void *s_tip;
static int note_save(void){return 0;}
static void note_paint(void){}
static const char *dm_t(const char *a,const char *b){(void)b;return a;}
static void lv_label_set_text(void *p,const char *s){(void)p;(void)s;}
static void dm_show(int p){(void)p;}
''' + function(source("dm_ui_note.c"), "dm_note_add_voice") + r'''
int main(void){assert(dm_note_add_voice("这是一条超过十个汉字的长备忘内容") == -4);
assert(s_note_n==0);assert(dm_note_add_voice("买牛奶")==0);
assert(s_note_n==1 && strcmp(s_notes[0],"买牛奶")==0);return 0;}
''')

    def test_manual_medicine_save_failure_rolls_back(self):
        self.run_c(r'''
typedef void lv_event_t;
#define MED_MAX 8
typedef struct {char name[32];uint8_t times,hours[3],taken;uint16_t day;} med_item_t;
static med_item_t s_med[8];static int s_med_n=1,s_med_sel;
static void *s_med_tip;
static int med_today(void){return 123;}
static int med_save(void){return -1;}
static int dm_wifi_clock_valid(void){return 1;}
static void med_paint(void){}
static void lv_label_set_text(void *p,const char *s){(void)p;(void)s;}
''' + function(source("dm_ui_med.c"), "med_take_cb") + "\n" +
                   function(source("dm_ui_med.c"), "med_del_cb") + r'''
int main(void){s_med[0].times=3;s_med[0].day=123;
strcpy(s_med[0].name,"维生素");med_take_cb(NULL);assert(s_med[0].taken==0);
med_del_cb(NULL);assert(s_med_n==1 && strcmp(s_med[0].name,"维生素")==0);
return 0;}
''')

    def test_game_stats_do_not_write_on_every_tick(self):
        self.run_c(r'''
static int32_t s_game_ms,s_game_saved_ms;static int writes;
static void health_rollover_if_new_day(void){}
static int store_save(void){writes++;return 0;}
''' + function(source("dm_ui_health.c"), "dm_health_add_game_ms") + "\n" +
                   function(source("dm_ui_health.c"), "dm_health_flush_game") + r'''
int main(void){int i;for(i=0;i<239;i++)dm_health_add_game_ms(250);
assert(writes==0);dm_health_add_game_ms(250);assert(writes==1);
dm_health_add_game_ms(250);assert(writes==1);dm_health_flush_game();
assert(writes==2);return 0;}
''')

    def test_water_date_is_not_interpreted_as_a_cup_goal(self):
        part = function(source("dm_proactive.c"), "water_due").replace(
            '"/data/deskmate_water.txt"', '"water.txt"')
        self.run_c(part + r'''
int main(void){time_t now=time(NULL);struct tm current;FILE *f;
localtime_r(&now,&current);int day=current.tm_yday+current.tm_year*1000;
f=fopen("water.txt","w");fprintf(f,"8 45 %ld %d\n",(long)now-1,day);fclose(f);
assert(water_due()==1);
f=fopen("water.txt","w");fprintf(f,"8 45 %ld %d\n",(long)now-1,day-1);fclose(f);
assert(water_due()==0);return 0;}
''')

    def test_audio_busy_does_not_consume_todo_notification(self):
        text = source("dm_ui_remind.c")
        self.run_c(self.reminders() + r'''
#include "dm_ai_permissions.h"
static int beep_calls;
static int dm_voice_beep(void){return ++beep_calls <= 14 ? -EBUSY : 0;}
static int dm_wifi_connected(void){return 0;}
bool dm_ai_permission_allowed(dm_ai_permission_t p){(void)p;return true;}
static int dm_voice_tts_play_private(const char *s){(void)s;return -1;}
static void dm_vol_alarm_begin(void){}
static void dm_vol_alarm_end(void){}
static int fake_sleep(unsigned int n){(void)n;return 0;}
#define usleep fake_sleep
''' + function(text, "reminder_speech_current") + "\n" +
                   function(text, "reminder_alarm_is_ringing") + "\n" +
                   function(text, "reminder_speech_worker") + r'''
int main(void){reminder_speech_t *speech=calloc(1,sizeof(*speech));
assert(speech);reminder_speech_worker(speech);
assert(beep_calls==15 && s_voice_done);return 0;}
''')

    def test_completed_capacity_reclaimed_without_dropping_pending_items(self):
        text = source("dm_ui_remind.c")
        self.run_c(self.reminders() + r'''
static int save_rc;
static int reminder_save(void){return save_rc;}
static void reminder_paint(void){}
''' + function(text, "reminder_copy_text") + "\n" +
                   function(text, "reminder_append") + r'''
int main(void){int i;s_count=REMINDER_MAX;s_alert_idx=0;
for(i=0;i<s_count;i++){s_items[i].fired=1;s_items[i].due=time(NULL)-1;}
s_items[1].pending=1;
assert(reminder_append(REMINDER_TODO,"new task",time(NULL)+100,0,0,0)==0);
assert(s_count==REMINDER_MAX && strcmp(s_items[2].text,"new task")==0);
assert(s_items[1].pending && s_items[0].fired);
save_rc=-1;assert(reminder_append(REMINDER_TODO,"failed task",time(NULL)+100,0,0,0)==-3);
assert(s_count==REMINDER_MAX && !s_items[3].text[0] && s_items[3].fired);
return 0;}
''')

    def test_permission_defaults_persistence_and_failed_save(self):
        implementation = source("dm_ai_permissions.c").replace(
            "#include <nuttx/config.h>", "")
        implementation = implementation.replace(
            '"/data/flowdesk_ai_permissions.txt"', '"permissions.txt"')
        implementation = implementation.replace('mkdir("/data", 0755)',
                                                'mkdir("data", 0755)')
        self.run_c(r'''
#define CONFIG_DESKMATE_APP 1
static int fail_save;
static int test_rename(const char *a,const char *b)
{if(fail_save)return -1;return rename(a,b);}
#define rename test_rename
''' + implementation + r'''
int main(void){int i;FILE *f;
dm_ai_permissions_init();
for(i=0;i<DM_AI_PERMISSION_COUNT;i++)assert(dm_ai_permission_allowed(i));
assert(dm_ai_permission_set(DM_AI_NOTE,false)==0);
assert(!dm_ai_permission_allowed(DM_AI_NOTE));
assert(dm_ai_permission_allowed(DM_AI_ALARM));
dm_ai_permissions_init();assert(!dm_ai_permission_allowed(DM_AI_NOTE));
fail_save=1;assert(dm_ai_permission_set(DM_AI_NOTE,true)==-1);
assert(!dm_ai_permission_allowed(DM_AI_NOTE));fail_save=0;
assert(dm_ai_permission_set(DM_AI_LOCAL,false)==0);
for(i=0;i<DM_AI_PERMISSION_COUNT;i++)assert(!dm_ai_permission_allowed(i));
assert(dm_ai_permission_enabled(DM_AI_ALARM));
assert(dm_ai_permission_set(DM_AI_LOCAL,true)==0);
assert(dm_ai_permission_allowed(DM_AI_ALARM));
assert(!dm_ai_permission_allowed(DM_AI_NOTE));
assert(!dm_ai_permission_allowed(DM_AI_PERMISSION_COUNT));
assert(dm_ai_permission_set(DM_AI_PERMISSION_COUNT,true)==-1);
f=fopen("permissions.txt","w");fputs("broken configuration",f);fclose(f);
dm_ai_permissions_init();
for(i=0;i<DM_AI_PERMISSION_COUNT;i++)assert(!dm_ai_permission_allowed(i));
return 0;}
''')

    def test_permission_denials_precede_all_intent_side_effects(self):
        text = source("dm_assistant.c")
        parser = self.parser()
        extra_pages = ["PAGE_ALARM", "PAGE_CHAT", "PAGE_HEALTH", "PAGE_MBTI",
                       "PAGE_MED", "PAGE_MOOD_LOG", "PAGE_TODO", "PAGE_NOTE",
                       "PAGE_COUNTDOWN", "PAGE_WATER"]
        for page in extra_pages:
            if page not in parser.split("} dm_page_t;")[0]:
                parser = parser.replace("} dm_page_t;", "," + page + "} dm_page_t;")
        functions = (function(source("dm_ai_data.c"), "dm_ai_data_read_permission") + "\n" +
                     function(source("dm_ai_data.c"), "dm_ai_data_delete_permission") + "\n" +
                     function(text, "intent_permission") + "\n" +
                     function(text, "intent_check_permission") + "\n" +
                     function(text, "run_intent"))
        self.run_c('#include "dm_ai_permissions.h"\n' + parser + r'''
static bool allowed[DM_AI_PERMISSION_COUNT];static int effects,jumps;static dm_page_t last_page;
static int todo_saves,note_saves,todo_delay,backend_rc;static char todo_title[256];
bool dm_ai_permission_allowed(dm_ai_permission_t p)
{return p>=0 && p<DM_AI_PERMISSION_COUNT && allowed[DM_AI_LOCAL] && allowed[p];}
const char *dm_ai_permission_name(dm_ai_permission_t p){(void)p;return "permission";}
static int dm_show(dm_page_t page){effects++;jumps++;last_page=page;return 0;}
static int dm_focus_start_seconds(){effects++;return 0;}
int dm_ai_read_local(dm_ai_read_kind_t k,char *out,size_t cap)
{(void)k;effects++;snprintf(out,cap,"LOCAL_RECORD_SECRET");return 0;}
int dm_ai_read_health(dm_ai_health_query_t q,char *out,size_t cap)
{(void)q;effects++;snprintf(out,cap,"HEALTH_RECORD_SECRET");return backend_rc;}
int dm_ai_delete_data(const dm_ai_delete_request_t *r)
{(void)r;effects++;return backend_rc;}
static int dm_todo_add_voice(const char *name,int delay)
{effects++;todo_saves++;todo_delay=delay;snprintf(todo_title,sizeof(todo_title),"%s",name);return backend_rc;}
static int dm_todo_add_at_voice(){effects++;return 0;}
static int dm_alarm_add_voice(){effects++;return 0;}
static int dm_alarm_add_after_voice(){effects++;return 0;}
static int dm_med_voice_take(){effects++;return 0;}
static int dm_countdown_add_voice(){effects++;return 0;}
static int dm_note_add_voice(){effects++;note_saves++;return 0;}
static int dm_health_record_mood_voice(){effects++;return 0;}
static void dm_health_voice_advice(const char *s,char *b,int cap)
{(void)s;(void)cap;b[0]=0;effects++;}
''' + functions + r'''
int main(void){assistant_intent_t intent;char reply[512];int type,i;
for(i=0;i<DM_AI_PERMISSION_COUNT;i++)allowed[i]=true;
assert(parse_intent("30分钟之后有个会，帮我记一下",&intent));
assert(run_intent(&intent,reply,sizeof(reply))==0);
assert(todo_saves==1 && note_saves==0 && todo_delay==1800);
assert(strcmp(todo_title,"开会")==0 && strstr(reply,"已添加待办"));
allowed[DM_AI_TODO]=false;effects=0;
assert(run_intent(&intent,reply,sizeof(reply))==-2 && effects==0);
assert(todo_saves==1 && note_saves==0);
record_intent_result(&intent,-2,reply);
assert(audit_result==DM_AI_LOG_DENIED && audit_permission==DM_AI_TODO);
assert(audit_reason==DM_AI_LOG_REASON_PERMISSION);allowed[DM_AI_TODO]=true;
backend_rc=-3;assert(run_intent(&intent,reply,sizeof(reply))==-1);
record_intent_result(&intent,-1,reply);
assert(audit_result==DM_AI_LOG_FAILED && audit_reason==DM_AI_LOG_REASON_SAVE && audit_rc==-3);
backend_rc=-2;assert(run_intent(&intent,reply,sizeof(reply))==-1);
record_intent_result(&intent,-1,reply);
assert(audit_result==DM_AI_LOG_FAILED && audit_reason==DM_AI_LOG_REASON_FULL);
backend_rc=0;
for(type=INTENT_MED_OPEN;type<=INTENT_TIME_ERROR;type++)
{
  memset(&intent,0,sizeof(intent));intent.type=type;intent.game_page=PAGE_2048;
  dm_ai_permission_t permission=intent_permission(&intent);
  if(permission==DM_AI_PERMISSION_COUNT)continue;
  allowed[permission]=false;effects=0;
  assert(run_intent(&intent,reply,sizeof(reply))==-2);
  assert(effects==0 && strstr(reply,"权限"));allowed[permission]=true;
}
memset(&intent,0,sizeof(intent));intent.type=INTENT_FOCUS;intent.seconds=1200;
allowed[DM_AI_OPEN_APPS]=false;effects=0;
assert(run_intent(&intent,reply,sizeof(reply))==-2 && effects==0);
record_intent_result(&intent,-2,reply);
assert(audit_permission==DM_AI_OPEN_APPS && audit_result==DM_AI_LOG_DENIED);
allowed[DM_AI_OPEN_APPS]=true;assert(run_intent(&intent,reply,sizeof(reply))==0);
assert(effects==1);
intent.type=INTENT_GAME;intent.game_page=PAGE_GUESS;
allowed[DM_AI_GAME_GUESS]=false;effects=0;
assert(run_intent(&intent,reply,sizeof(reply))==-2 && effects==0);
allowed[DM_AI_GAME_GUESS]=true;
intent.type=INTENT_GAME_ASK;s_waiting_game_choice=1;
allowed[DM_AI_GAME_2048]=false;allowed[DM_AI_GAME_GUESS]=false;allowed[DM_AI_GAME_24]=false;
assert(run_intent(&intent,reply,sizeof(reply))==-1 && !s_waiting_game_choice);
intent.type=INTENT_GAME;intent.game_page=PAGE_GUESS;
allowed[DM_AI_GAME_GUESS]=true;allowed[DM_AI_LOCAL]=false;
assert(run_intent(&intent,reply,sizeof(reply))==-2 && effects==0);
intent.type=INTENT_DELETE_UNSUPPORTED;
assert(run_intent(&intent,reply,sizeof(reply))==0 && effects==0);
assert(strstr(reply,"不支持"));
allowed[DM_AI_LOCAL]=true;allowed[DM_AI_OPEN_APPS]=false;
for(int kind=0;kind<DM_AI_DATA_COUNT;kind++){
static const dm_page_t expected[]={PAGE_NOTE,PAGE_TODO,PAGE_ALARM,PAGE_COUNTDOWN,PAGE_HEALTH,PAGE_WATER,PAGE_MED};
memset(&intent,0,sizeof(intent));intent.type=INTENT_DELETE_DATA;intent.deletion.kind=kind;
dm_ai_permission_t read=dm_ai_data_read_permission(kind),del=dm_ai_data_delete_permission(kind);
allowed[read]=false;effects=0;
assert(run_intent(&intent,reply,sizeof(reply))==-2 && !effects);
record_intent_result(&intent,-2,reply);assert(audit_permission==read);
allowed[read]=true;allowed[del]=false;
assert(run_intent(&intent,reply,sizeof(reply))==-2 && !effects);allowed[del]=true;
assert(run_intent(&intent,reply,sizeof(reply))==-2 && !effects);
record_intent_result(&intent,-2,reply);assert(audit_permission==DM_AI_OPEN_APPS);
allowed[DM_AI_OPEN_APPS]=true;jumps=0;
assert(!run_intent(&intent,reply,sizeof(reply)) && effects==2 && jumps==1 && last_page==expected[kind]);
effects=0;jumps=0;
backend_rc=-3;assert(run_intent(&intent,reply,sizeof(reply))==-3);
assert(effects==1 && !jumps);
record_intent_result(&intent,-3,reply);
assert(audit_result==DM_AI_LOG_FAILED && audit_reason==DM_AI_LOG_REASON_SAVE);
assert(audit_action==DM_AI_LOG_DELETE_NOTE+kind);backend_rc=0;
const int failures[]={DM_AI_DATA_NOT_FOUND,DM_AI_DATA_AMBIGUOUS,DM_AI_DATA_BUSY,DM_AI_DATA_CLOCK};
for(unsigned f=0;f<sizeof(failures)/sizeof(failures[0]);f++){
backend_rc=failures[f];effects=0;jumps=0;
assert(run_intent(&intent,reply,sizeof(reply))==backend_rc && effects==1 && !jumps);}
backend_rc=0;allowed[DM_AI_OPEN_APPS]=false;
}
memset(&intent,0,sizeof(intent));intent.type=INTENT_READ_HEALTH;effects=0;
allowed[DM_AI_READ_HEALTH]=false;
assert(run_intent(&intent,reply,sizeof(reply))==-2 && !effects);
allowed[DM_AI_READ_HEALTH]=true;
assert(!run_intent(&intent,reply,sizeof(reply)) && effects==1);
return 0;}
''')

    def test_health_read_permission_blocks_actual_stats_reads(self):
        self.run_c(AUDIT_STUB + r'''
#include "dm_ai_permissions.h"
static bool companion=true,read_health;
static int reads,s_last_has_mood,s_last_has_beh;
static struct {int focus_done_min;} g_dm;
bool dm_ai_permission_allowed(dm_ai_permission_t p)
{return p==DM_AI_COMPANION ? companion : read_health;}
static void health_rollover_if_new_day(void){reads++;}
static int dm_health_score(void){reads++;return 50;}
static int dm_water_today_cups(void){reads++;return 3;}
static int dm_med_today_stats(int *a,int *b){reads++;*a=1;*b=1;return 0;}
static int health_voice_is_down(const char *s){(void)s;return 0;}
''' + function(source("dm_ui_health.c"), "dm_health_voice_advice") + r'''
int main(void){char reply[512];
dm_health_voice_advice("我很累",reply,sizeof(reply));assert(reads==0 && reply[0]);
assert(audit_calls==1 && audit_action==DM_AI_LOG_READ_HEALTH);
assert(audit_result==DM_AI_LOG_DENIED && audit_permission==DM_AI_READ_HEALTH);
companion=false;dm_health_voice_advice("我很累",reply,sizeof(reply));
assert(reads==0 && !reply[0]);
companion=true;read_health=true;dm_health_voice_advice("我很累",reply,sizeof(reply));
assert(reads>0 && audit_result==DM_AI_LOG_SUCCESS);return 0;}
''')

    def test_health_history_is_filtered_but_new_chat_keeps_memory(self):
        text = source("dm_chat_ai.c")
        history = text[text.index("#define HIST_TURN_MAX"):
                       text.index("static void extract_content(")]
        self.run_c(r'''
#include "dm_ai_permissions.h"
static bool health_allowed=true;
bool dm_ai_permission_allowed(dm_ai_permission_t p){(void)p;return health_allowed;}
''' + function(text, "json_escape") + "\n" + history + r'''
int main(void){char json[4096];
dm_chat_hist_push("old context","LOCAL_HEALTH_SECRET");
health_allowed=false;
dm_chat_hist_push("主动语音聊天","普通聊天回复");
dm_chat_hist_push_health("记录心情","NEW_LOCAL_HEALTH_SECRET");
assert(dm_chat_hist_json(json,sizeof(json))>0);
assert(!strstr(json,"LOCAL_HEALTH_SECRET"));
assert(strstr(json,"主动语音聊天") && strstr(json,"普通聊天回复"));
health_allowed=true;
assert(dm_chat_hist_json_for_cloud(json,sizeof(json),false)>0);
assert(!strstr(json,"LOCAL_HEALTH_SECRET"));
assert(dm_chat_hist_json(json,sizeof(json))>0);
assert(strstr(json,"LOCAL_HEALTH_SECRET"));return 0;}
''')

    def test_health_tts_blocked_without_disabling_regular_chat_speech(self):
        text = source("dm_voice.c")
        self.run_c(r'''
#include "dm_ai_permissions.h"
static pthread_mutex_t s_voice_api_lock=PTHREAD_MUTEX_INITIALIZER;
static bool allowed;
static int uploads;
bool dm_ai_permission_allowed(dm_ai_permission_t p){(void)p;return allowed;}
static int voice_tts_play_unlocked(const char *text,bool health)
{(void)text;if(health)assert(allowed);uploads++;return 0;}
''' + function(text, "dm_voice_tts_play_private") + "\n" +
                   function(text, "dm_voice_tts_play") + r'''
int main(void){assert(dm_voice_tts_play_private("本地健康记录")==2);
assert(uploads==0);
assert(dm_voice_tts_play("用户主动聊天的回复")==0 && uploads==1);
allowed=true;assert(dm_voice_tts_play_private("已授权的健康记录")==0);
assert(uploads==2);return 0;}
''')

    def test_permission_switch_rolls_back_after_save_failure(self):
        self.run_c(r'''
#include "dm_ai_permissions.h"
#define LV_STATE_CHECKED 1
typedef struct {bool checked;} lv_obj_t;
typedef struct {lv_obj_t *target;intptr_t permission;} lv_event_t;
static bool s_switch_sync;
static lv_obj_t status,*s_status=&status;
static char message[256];
static void *lv_event_get_user_data(lv_event_t *e){return (void *)e->permission;}
static lv_obj_t *lv_event_get_target(lv_event_t *e){return e->target;}
static bool lv_obj_has_state(lv_obj_t *o,int state){(void)state;return o->checked;}
static void lv_obj_add_state(lv_obj_t *o,int state){(void)state;o->checked=true;}
static void lv_obj_clear_state(lv_obj_t *o,int state){(void)state;o->checked=false;}
static void lv_label_set_text(lv_obj_t *o,const char *s){(void)o;snprintf(message,sizeof(message),"%s",s);}
int dm_ai_permission_set(dm_ai_permission_t p,bool enabled){(void)p;(void)enabled;return -1;}
bool dm_ai_permission_enabled(dm_ai_permission_t p){(void)p;return false;}
''' + function(source("dm_ui_permissions.c"), "permission_changed") + r'''
int main(void){lv_obj_t toggle={true};lv_event_t event={&toggle,DM_AI_READ_HEALTH};
permission_changed(&event);assert(!toggle.checked);
assert(strstr(message,"保存失败"));return 0;}
''')

    def test_voice_chat_stays_enabled_without_health_record_access(self):
        self.run_c(r'''
#include "dm_ai_permissions.h"
#include "dm_voice.h"
#define WAV_TTS "voice-out.wav"
static int s_st,handled,asr_calls,chat_calls,tts_calls,hist_calls,s_health_reply;
static dm_voice_failure_t s_last_failure;
static bool health_allowed;
bool dm_ai_permission_allowed(dm_ai_permission_t p){(void)p;return health_allowed;}
static int voice_prepare_wav(const char *s,const char **p){*p=s;return 0;}
static int asr_wav(const char *p,char *s,int cap)
{(void)p;asr_calls++;snprintf(s,cap,"用户主动说的话");return 0;}
int dm_assistant_intent_try(const char *s,char *out,int cap)
{(void)s;snprintf(out,cap,"本地健康结果");return handled;}
static void dm_chat_hist_push(const char *s,const char *r){(void)s;(void)r;hist_calls++;}
static void dm_chat_hist_push_health(const char *s,const char *r){(void)s;(void)r;hist_calls++;}
static int chat_reply(const char *s,char *r,int cap,bool *private_context)
{(void)s;*private_context=false;chat_calls++;snprintf(r,cap,"普通聊天回复");return 0;}
static int voice_play_wake_samples_unlocked(void){return 0;}
static const char *dm_voice_profile_api_id(void){return "voice";}
static int tts_synth(const char *s,const char *p,const char *v,bool health)
{(void)s;(void)p;(void)v;if(health)assert(health_allowed);tts_calls++;return 0;}
static int play_wav(const char *p){(void)p;return 0;}
''' + function(source("dm_voice.c"), "voice_asr_chat_tts_unlocked") + r'''
int main(void){char reply[512];
assert(voice_asr_chat_tts_unlocked("user-audio.wav",reply,sizeof(reply))==0);
assert(asr_calls==1 && chat_calls==1 && tts_calls==1);
handled=3;
assert(voice_asr_chat_tts_unlocked("user-audio.wav",reply,sizeof(reply))==2);
assert(asr_calls==2 && chat_calls==1 && tts_calls==1);
health_allowed=true;
assert(voice_asr_chat_tts_unlocked("user-audio.wav",reply,sizeof(reply))==0);
assert(tts_calls==2);
handled=4;
assert(voice_asr_chat_tts_unlocked("user-audio.wav",reply,sizeof(reply))==3);
assert(tts_calls==2 && hist_calls==2 && chat_calls==1);
handled=5;assert(voice_asr_chat_tts_unlocked("query.wav",reply,sizeof(reply))==0);
assert(s_health_reply && tts_calls==3 && hist_calls==2 && chat_calls==1);
health_allowed=false;assert(voice_asr_chat_tts_unlocked("query.wav",reply,sizeof(reply))==2);
assert(tts_calls==3 && hist_calls==2 && chat_calls==1);return 0;}
''')

    def test_actual_chat_body_omits_old_local_health_context(self):
        text = source("dm_chat_ai.c")
        state = text[text.index("static char s_last_reply"):
                     text.index("static const char *const s_local_lines")]
        history = text[text.index("#define HIST_TURN_MAX"):
                       text.index("static void extract_content(")]
        self.run_c(r'''
#include "dm_ai_permissions.h"
#define CHAT_MAX_REPLY 240
#define CHAT_REQ_PATH "request.json"
#define CHAT_RSP_PATH "response.json"
#define MIMO_BASE "mock://cloud"
#define MIMO_MODEL "model"
static bool health_allowed=true;static int requests,local_mode;static char sent[4096];
bool dm_ai_permission_allowed(dm_ai_permission_t p){(void)p;return health_allowed;}
static const char *local_reply(const char *s){(void)s;return "fallback";}
static int dm_wifi_connected(void){return 1;}
static int dm_wifi_wait_time_sync(int t){(void)t;return 0;}
static int dm_assistant_intent_try(const char *s,char *r,int cap)
{(void)s;if(local_mode){snprintf(r,cap,local_mode==2?"LOCAL_HEALTH_QUERY_SECRET":"LOCAL_LIFE_RECORD_SECRET");return local_mode==2?5:4;}return 0;}
static int run_curl_command(const char *url,const char *request,const char *response,int timeout)
{(void)url;(void)response;(void)timeout;FILE *f=fopen(request,"r");assert(f);
size_t n=fread(sent,1,sizeof(sent)-1,f);sent[n]=0;fclose(f);requests++;return 0;}
static void extract_content(const char *p,char *r,size_t cap)
{(void)p;snprintf(r,cap,"normal reply");}
''' + state + function(text, "json_escape") + "\n" + history +
                   function(text, "dm_chat_ai_ask") + r'''
int main(void){strcpy(s_persona_ctx,"LOCAL_HEALTH_CONTEXT_SECRET");
dm_chat_hist_push("old health question","LOCAL_HEALTH_HISTORY_SECRET");
health_allowed=false;
assert(dm_chat_ai_ask("用户主动说出的内容"));assert(requests==1);
assert(!strstr(sent,"LOCAL_HEALTH_CONTEXT_SECRET"));
assert(!strstr(sent,"LOCAL_HEALTH_HISTORY_SECRET"));
assert(strstr(sent,"用户主动说出的内容"));
assert(dm_chat_ai_ask("继续普通聊天"));assert(requests==2);
assert(strstr(sent,"normal reply"));
local_mode=1;assert(strstr(dm_chat_ai_ask("读一下备忘"),"LOCAL_LIFE_RECORD_SECRET"));
assert(dm_chat_ai_local_reply() && requests==2);
char history_json[4096];dm_chat_hist_json(history_json,sizeof(history_json));
assert(!strstr(history_json,"LOCAL_LIFE_RECORD_SECRET"));
local_mode=2;assert(strstr(dm_chat_ai_ask("最近心情什么时候"),"LOCAL_HEALTH_QUERY_SECRET"));
assert(s_last_reply_health && !dm_chat_ai_local_reply() && requests==2);
dm_chat_hist_json(history_json,sizeof(history_json));assert(!strstr(history_json,"LOCAL_HEALTH_QUERY_SECRET"));
local_mode=0;dm_chat_ai_ask("继续聊天");
assert(requests==3 && !strstr(sent,"LOCAL_LIFE_RECORD_SECRET") && !strstr(sent,"LOCAL_HEALTH_QUERY_SECRET"));return 0;}
''')

    def test_malformed_permission_masks_cannot_grant_access(self):
        implementation = source("dm_ai_permissions.c").replace(
            "#include <nuttx/config.h>", "")
        implementation = implementation.replace(
            '"/data/flowdesk_ai_permissions.txt"', '"permissions.txt"')
        implementation = implementation.replace('mkdir("/data", 0755)',
                                                'mkdir("data", 0755)')
        self.run_c('#define CONFIG_DESKMATE_APP 1\n' + implementation + r'''
int main(void){const char *invalid[]={"-0","+0","2147483648","4294967296","0 extra"};
int i,j;FILE *f;
for(i=0;i<5;i++){
f=fopen("permissions.txt","w");fprintf(f,"FLOWDESK_AI_PERMISSIONS_V1 %s\n",invalid[i]);fclose(f);
dm_ai_permissions_init();
for(j=0;j<DM_AI_PERMISSION_COUNT;j++)assert(!dm_ai_permission_allowed(j));
}return 0;}
''')

    def test_health_action_result_does_not_return_record_details_when_denied(self):
        text = source("dm_assistant.c")
        self.run_c('#include "dm_ai_permissions.h"\n' + self.parser() + r'''
static char answer[512];static int run_rc;
bool dm_ai_permission_allowed(dm_ai_permission_t p){(void)p;return false;}
static int run_intent(const assistant_intent_t *intent,char *reply,size_t cap)
{(void)intent;snprintf(reply,cap,"%s",run_rc==-2 ? "已关闭权限" : "PRIVATE_MEDICATION_COUNT_2_OF_3");return run_rc;}
''' + function(text, "dm_assistant_intent_try") + "\n" +
                   function(text, "dm_assistant_intent_tick") + r'''
static void *request(void *p){(void)p;assert(dm_assistant_intent_try("我刚吃了阿莫西林",answer,sizeof(answer))==3);return NULL;}
int main(void){int attempt,state;pthread_t worker;
for(attempt=0;attempt<3;attempt++){
run_rc=-attempt;answer[0]=0;
pthread_create(&worker,NULL,request,NULL);
do{pthread_mutex_lock(&s_intent_lock);state=s_intent_active;pthread_mutex_unlock(&s_intent_lock);}while(!state);
dm_assistant_intent_tick();pthread_join(worker,NULL);
assert(answer[0] && !strstr(answer,"PRIVATE_MEDICATION"));
if(attempt==2)assert(strstr(answer,"权限"));
}return 0;}
''')


if __name__ == "__main__":
    unittest.main(verbosity=2)
