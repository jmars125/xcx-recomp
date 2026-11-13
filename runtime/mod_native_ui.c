#include "message_text.h"
#include "mod_native_ui.h"
#include "mod_settings.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
static atomic_flag guard=ATOMIC_FLAG_INIT;static int toast_waiting;
static ModNativeUi registered;static int supported;static uint64_t epoch=1;
static atomic_uint published;static atomic_int session_up;static void (*wake)(void);
static void lock(void){while(atomic_flag_test_and_set_explicit(&guard,memory_order_acquire)){};}
static void unlock(void){atomic_flag_clear_explicit(&guard,memory_order_release);}
void mod_native_ui_set_wake(void (*callback)(void)){lock();wake=callback;unlock();}
void mod_native_ui_reset(void){lock();atomic_store(&session_up,0);toast_waiting=0;registered=(ModNativeUi){0};supported=0;atomic_store(&published,0);if(epoch!=UINT64_MAX)++epoch;unlock();}
void mod_native_ui_support(int yes){lock();supported=epoch==UINT64_MAX?-1:yes;if(supported<=0)atomic_store(&published,0);unlock();}
const char *mod_native_ui_register(const ModNativeUi *r){
    const char *error=NULL;lock();
    if(!supported)error="unsupported";else if(supported<0)error="unavailable";else if(registered.owner[0])error="full";
    else{registered=*r;registered.epoch=epoch;atomic_store(&published,r->features);}
    unlock();return error;
}
unsigned mod_native_ui_features(void){return atomic_load_explicit(&published,memory_order_acquire);}
static int healthy(const ModNativeUi *r){ModSnapshot s;mod_ui_snapshot(&s);for(int i=0;i<s.count;++i)if(!strcmp(r->owner,s.mods[i].id)&&s.mods[i].status==MOD_LOADED)return 1;return 0;}
int mod_native_ui_confirm_text(unsigned option,const char *text){
    if(option>=MOD_NATIVE_ACTIONS)return 0;
    lock();ModNativeUi r=registered;int ok=supported>0&&r.epoch==epoch;unlock();
    if(!ok||!(r.features&(option<MOD_NATIVE_OPTIONS?MOD_NATIVE_SQUAD_SELECT:MOD_NATIVE_DETAILS))||!r.owner[0]||!r.action[option][0]||!healthy(&r))return 0;
    // An owner that faulted or stopped never gets the action; the screen stays inert instead.
    if(!mod_ui_native_intent_submit_text(r.owner,r.action[option],text))return 0;
    lock();void (*notify)(void)=wake;unlock();if(notify)notify();return 1;
}
int mod_native_ui_confirm(unsigned option){return mod_native_ui_confirm_text(option,NULL);}
int mod_native_ui_has_action(unsigned option){
    if(option>=MOD_NATIVE_ACTIONS)return 0;
    lock();ModNativeUi r=registered;int ok=supported>0&&r.epoch==epoch;unlock();
    return ok&&(r.features&(option<MOD_NATIVE_OPTIONS?MOD_NATIVE_SQUAD_SELECT:MOD_NATIVE_DETAILS))&&r.owner[0]&&r.action[option][0]&&healthy(&r);
}
static atomic_uint answer;static char reported[MOD_NATIVE_TEXT_MAX+1];static char reported_code[MOD_NATIVE_CODE_MAX+1];static int reported_mode=MOD_NATIVE_MODE_NONE;static char reported_label[MOD_NATIVE_LABEL_MAX+1],reported_status[MOD_NATIVE_STATUS_MAX+1];static atomic_uint status_version;
int mod_native_ui_text_valid(const char *text){return message_text_valid(text);}
void mod_native_ui_begin(void){lock();reported[0]=0;reported_code[0]=0;reported_mode=MOD_NATIVE_MODE_NONE;reported_label[0]=0;reported_status[0]=0;atomic_fetch_add(&status_version,1);unlock();atomic_store(&answer,MOD_NATIVE_PENDING);}
int mod_native_ui_report_message(const char *owner,unsigned status,const char *text){
    if(status!=MOD_NATIVE_OK&&status!=MOD_NATIVE_FAILED)return 0;
    if(text&&(status!=MOD_NATIVE_FAILED||!message_text_valid(text)))return 0; // a text explains a failure
    lock();int ok=supported>0&&registered.epoch==epoch&&registered.owner[0]&&!strcmp(registered.owner,owner);
    if(ok){if(text)snprintf(reported,sizeof reported,"%s",text);else reported[0]=0;reported_code[0]=0;reported_mode=MOD_NATIVE_MODE_NONE;reported_label[0]=0;reported_status[0]=0;atomic_fetch_add(&status_version,1);}
    unlock();
    if(ok)atomic_store(&answer,status); // the text first: the status publishes it
    return ok;
}
int mod_native_ui_report(const char *owner,unsigned status){return mod_native_ui_report_message(owner,status,NULL);}
int mod_native_ui_code_valid(const char *code){
    size_t n=code?strlen(code):0;if(!n||n>MOD_NATIVE_CODE_MAX)return 0;
    for(size_t i=0;i<n;++i){char c=code[i];if(!((c>='0'&&c<='9')||(c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='-'))return 0;}
    return 1;
}
int mod_native_ui_label_valid(const char *text){return message_text_short_valid(text,MOD_NATIVE_LABEL_MAX);}
int mod_native_ui_status_valid(const char *text){return message_text_short_valid(text,MOD_NATIVE_STATUS_MAX);}
int mod_native_ui_report_details(const char *owner,const char *code,int mode,const char *label,const char *status){
    if((code&&!mod_native_ui_code_valid(code))||mode<MOD_NATIVE_MODE_NONE||mode>MOD_NATIVE_MODE_IN_SQUAD)return 0;
    if((label&&!mod_native_ui_label_valid(label))||(status&&!mod_native_ui_status_valid(status)))return 0;
    lock();int ok=supported>0&&registered.epoch==epoch&&registered.owner[0]&&!strcmp(registered.owner,owner)&&(registered.features&MOD_NATIVE_DETAILS);
    if(ok){reported[0]=0;snprintf(reported_code,sizeof reported_code,"%s",code?code:"");reported_mode=mode;
     snprintf(reported_label,sizeof reported_label,"%s",label?label:"");snprintf(reported_status,sizeof reported_status,"%s",status?status:"");atomic_fetch_add(&status_version,1);}
    unlock();
    if(ok)atomic_store(&answer,MOD_NATIVE_OK); // the details first: the status publishes them
    return ok;
}
int mod_native_ui_details_status(const char *owner,const char *status){
    if(!mod_native_ui_status_valid(status))return 0;
    lock();int ok=supported>0&&registered.epoch==epoch&&registered.owner[0]&&!strcmp(registered.owner,owner)&&(registered.features&MOD_NATIVE_DETAILS);
    if(ok){snprintf(reported_status,sizeof reported_status,"%s",status);atomic_fetch_add(&status_version,1);}
    unlock();return ok;
}
unsigned mod_native_ui_details_text(char *label,size_t label_cap,char *status,size_t status_cap){
    lock();if(label_cap)snprintf(label,label_cap,"%s",reported_label);if(status_cap)snprintf(status,status_cap,"%s",reported_status);unsigned v=atomic_load(&status_version);unlock();return v;
}
int mod_native_ui_set_session(const char *owner,int active){
    lock();int ok=supported>0&&registered.epoch==epoch&&registered.owner[0]&&!strcmp(registered.owner,owner)&&(registered.features&MOD_NATIVE_DETAILS);
    if(ok)atomic_store(&session_up,active!=0);
    unlock();return ok;
}
int mod_native_ui_session_active(void){return atomic_load(&session_up);}
int mod_native_ui_details(char *code,size_t cap){
    lock();int mode=reported_mode;if(cap)snprintf(code,cap,"%s",reported_code);unlock();return mode;
}
static char toast[MESSAGE_TOAST_MAX+1];
int mod_native_ui_toast_valid(const char *text){return message_text_toast_valid(text);}
int mod_native_ui_toast(const char *owner,const char *text){
    if(!message_text_toast_valid(text))return 0;
    lock();int ok=supported>0&&registered.epoch==epoch&&registered.owner[0]&&!strcmp(registered.owner,owner)&&(registered.features&MOD_NATIVE_SQUAD_SELECT);
    if(ok){snprintf(toast,sizeof toast,"%s",text);toast_waiting=1;} // the latest request replaces one that has not been shown yet
    unlock();return ok;
}
int mod_native_ui_take_toast(char *out,size_t cap){
    if(!cap)return 0;lock();int had=toast_waiting;if(had){snprintf(out,cap,"%s",toast);toast_waiting=0;}unlock();return had;
}
unsigned mod_native_ui_result(void){return atomic_load(&answer);}
void mod_native_ui_message(char *out,size_t cap){if(!cap)return;lock();snprintf(out,cap,"%s",reported);unlock();}
void mod_native_ui_invalidate(void){lock();atomic_store(&session_up,0);toast_waiting=0;if(epoch!=UINT64_MAX)++epoch;if(epoch==UINT64_MAX)supported=-1;atomic_store(&published,0);unlock();}
