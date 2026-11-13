// Synthetic fixture for the native Online-screen hooks: a fake squad screen view, a scripted pad and a recording rt_call.
// No game bytes and no generated code; only the adapter, the registry and the intent queue are real.
#include "host.h"
#include "message_text.h"
#include "mod_native_ui.h"
#include "mod_settings.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
volatile int g_fnprof;const char rt_game_name[]="2.0.0";
void fnprof_hit(uint64_t a,uint64_t b,uint64_t c){(void)a;(void)b;(void)c;}
void rt_dispatch(Cpu *c){(void)c;abort();}
void rt_log(const char *s,...){(void)s;}
static uint64_t fake_now=1000;uint64_t kern_now_ns(void){return fake_now;} // the toast hold is real time; the fixture moves it by hand
#define TOAST_NS 4000000000ull
uint32_t mem_query(uint64_t p,RtMemoryInfo *i){i->base=p&~0xfffull;i->size=0x1000;i->perm=3;return 0;}
extern uint64_t rt_native_ui_busy_min_ns;
extern uint64_t (*rt_native_ui_read64)(uint64_t);
static uint64_t fake_guest_word(uint64_t a){return a==0x7001dfb000ull+0x608?0x7777:a==0x7001dc1410ull?0x1234:a==0x7001d54000ull+0xe40?0x5000:a==0x5000?0x9999:0;} // the dialog holder, and the message manager behind two pointers
static unsigned wakes;static void wake(void){wakes++;}
// A fake guest heap holding the message table's spare rows (the adapter searches it instead of the address space).
extern int (*rt_native_ui_next_region)(uint64_t,uint64_t *,uint64_t *);
static _Alignas(4096) uint8_t fake_heap[8192];static unsigned region_calls;
static int fake_region(uint64_t after,uint64_t *b,uint64_t *s){region_calls++;uint64_t lo=(uint64_t)fake_heap,hi=lo+sizeof fake_heap;if(after>=hi)return 0;*b=after>lo?after:lo;*s=hi-*b;return 1;}
// Two synthetic BDAT tables of the shape message_text.h looks for (made-up strings, no game text): the main table with the pool rows and
// the timer label row, the notice table with the toast row. The rows have the lengths of the real rows and the helpers are told each
// row's ID, length and hash. A string "neighbour" follows the last string of the main table.
static uint64_t row_pos[MESSAGE_ROWS];static char fake_original[MESSAGE_ROWS][MESSAGE_ORIGINAL_MAX];
static size_t put_u32(uint8_t *p,uint32_t v){memcpy(p,&v,4);return 4;}
// A table at `t` with `rows` rows (first ID 1) whose row `ids[i]` has string `strs[i]`; returns the table's size and each string's offset.
static size_t build_table(uint8_t *t,uint32_t rows,unsigned n,const uint32_t *ids,const char *const *strs,uint32_t *off,const char *tail){
 uint32_t str_off=64+rows*10,at=1;memset(t,0,str_off);memcpy(t,"BDAT",4);put_u32(t+4,0x3004);put_u32(t+8,3);put_u32(t+12,rows);put_u32(t+16,1);
 put_u32(t+24,48);put_u32(t+32,64);put_u32(t+36,10);put_u32(t+40,str_off);t[48]=9;t[51]=2;t[54]=7;t[str_off]=0;
 for(unsigned i=0;i<n;++i){off[i]=at;memcpy(t+str_off+at,strs[i],strlen(strs[i])+1);put_u32(t+64+(ids[i]-1)*10+6,at);at+=(uint32_t)strlen(strs[i])+1;}
 if(tail){memcpy(t+str_off+at,tail,strlen(tail)+1);at+=(uint32_t)strlen(tail)+1;}
 put_u32(t+44,at);return str_off+at;}
static void stock_table(void){
 static const unsigned lens[MESSAGE_ROWS]={78,102,75,46,13};static const uint32_t main_ids[4]={2,3,4,5},notice_ids[1]={3};
 memset(fake_heap,0,sizeof fake_heap);message_text_test_table(MESSAGE_TABLE_MAIN,8,1);message_text_test_table(MESSAGE_TABLE_NOTICE,6,1);
 for(unsigned k=0;k<MESSAGE_ROWS;++k){memset(fake_original[k],'a'+(int)k,lens[k]);fake_original[k][lens[k]]=0;memcpy(fake_original[k],"row ",4);fake_original[k][4]='0'+(char)k;
  message_text_test_row(k,k==MESSAGE_TOAST_ROW?MESSAGE_TABLE_NOTICE:MESSAGE_TABLE_MAIN,k==MESSAGE_TOAST_ROW?notice_ids[0]:main_ids[k<MESSAGE_TOAST_ROW?k:3],lens[k],message_text_hash((const uint8_t *)fake_original[k],lens[k]));}
 const char *notice_strs[1]={fake_original[MESSAGE_TOAST_ROW]},*main_strs[4]={fake_original[0],fake_original[1],fake_original[2],fake_original[MESSAGE_LABEL_ROW]};
 uint32_t off[4],noff[1];uint8_t *nt=fake_heap+16;size_t nsize=build_table(nt,6,1,notice_ids,notice_strs,noff,NULL);
 uint8_t *mt=fake_heap+16+((nsize+15)&~(size_t)15);build_table(mt,8,4,main_ids,main_strs,off,"neighbour");
 uint32_t str_off=64+8*10;row_pos[MESSAGE_TOAST_ROW]=(uint64_t)(nt+64+6*10+noff[0]);
 for(unsigned k=0;k<MESSAGE_TOAST_ROW;++k)row_pos[k]=(uint64_t)(mt+str_off+off[k]);row_pos[MESSAGE_LABEL_ROW]=(uint64_t)(mt+str_off+off[3]);}
static int row_says(unsigned k,const char *text){return message_text_holds((const uint8_t *)(uintptr_t)row_pos[k],fake_original[k],message_text_row(k)->length,text);}
int rt_native_ui_capable(void);int rt_native_ui_hook(unsigned,Cpu *);
static unsigned toast_posts;static uint64_t toast_manager,toast_id;
static uint32_t pad;static unsigned pad_reads,animations,answer_polls,answer_consumed,reroutes,unselects;static uint64_t animated_component;static int32_t dialog_answer;
static unsigned kbd_requests;static uint64_t kbd_parent;static unsigned detail_opens,detail_clicks,status_sets,busy_ons,busy_offs,outs,ins,status_writes;static uint64_t busy_object,pane_view,pane_name,setter_arg;static char status_text[16];static uint64_t detail_parent,detail_mode,status_manager,last_status,fake_status;static char detail_code[16];
uint64_t rt_call(Cpu *c,uint64_t fn,int n,...){
 (void)c;(void)n;
 if(fn==0x7000000000ull+0x40b93c){pad_reads++;return pad;}
 if(fn==0x7000000000ull+0x55faf4){animations++;return 0;}
 if(fn==0x7000000000ull+0x717460){reroutes++;return 0;} // the game's own handler answering as second card
 if(fn==0x7000000000ull+0x55f8d4){va_list ap;va_start(ap,n);(void)va_arg(ap,uint64_t);uint64_t name=va_arg(ap,uint64_t);va_end(ap);if(name==0x7000000000ull+0x177b865)outs++;else if(name==0x7000000000ull+0x178fc1b)ins++;else unselects++;return 0;}
 if(fn==0x7000000000ull+0x928f20){va_list ap;va_start(ap,n);toast_manager=va_arg(ap,uint64_t);toast_id=va_arg(ap,uint64_t);va_end(ap);toast_posts++;return 0;}
 if(fn==0x7000000000ull+0x92a0d4){answer_polls++;return (uint32_t)dialog_answer;}
 if(fn==0x7000000000ull+0x92a0a4){answer_consumed++;return 0;}
 if(fn==0x7000000000ull+0x926058||fn==0x7000000000ull+0x926214)return 0; // the text variant: made and released
 if(fn==0x7000000000ull+0x9262cc){va_list ap;va_start(ap,n);(void)va_arg(ap,uint64_t);uint64_t s=va_arg(ap,uint64_t);va_end(ap);strncpy(status_text,(const char *)(uintptr_t)s,sizeof status_text-1);status_writes++;return 0;}
 if(fn==0x7000000000ull+0x55f16c){va_list ap;va_start(ap,n);pane_view=va_arg(ap,uint64_t);pane_name=va_arg(ap,uint64_t);va_end(ap);return 0x4242;} // the pane found by name
 if(fn==0x7000000000ull+0x915110){va_list ap;va_start(ap,n);setter_arg=va_arg(ap,uint64_t);va_end(ap);return 0x4343;}
 if(fn==0x7000000000ull+0x9172a0)return 0;
 if(fn==0x7000000000ull+0x8f09a8){va_list ap;va_start(ap,n);detail_parent=va_arg(ap,uint64_t);detail_mode=va_arg(ap,uint64_t);uint64_t s=va_arg(ap,uint64_t);va_end(ap);strncpy(detail_code,(const char *)(uintptr_t)s,sizeof detail_code-1);detail_opens++;return 0;} // the Details screen's open
 if(fn==0x7000000000ull+0x8f0c84){detail_clicks++;return 0;}
 if(fn==0x7000000000ull+0x8f0c08){va_list ap;va_start(ap,n);kbd_parent=va_arg(ap,uint64_t);va_end(ap);kbd_requests++;return 0;} // the game's passcode keyboard request
 if(fn==0x7000000000ull+0x40f1dc||fn==0x7000000000ull+0x40f1ec){va_list ap;va_start(ap,n);busy_object=va_arg(ap,uint64_t);va_end(ap);if(fn==0x7000000000ull+0x40f1dc)busy_ons++;else busy_offs++;return 0;}
 if(fn==0x7000000000ull+0x928744)return fake_status;
 if(fn==0x7000000000ull+0x928734){va_list ap;va_start(ap,n);status_manager=va_arg(ap,uint64_t);last_status=va_arg(ap,uint64_t);va_end(ap);status_sets++;return 0;}
 abort();
}
static _Alignas(4096) uint8_t view_page[4096];
static void publish(int status){ModView v={0};strcpy(v.id,"alpha");v.status=status;v.button_count=4;strcpy(v.buttons[0].id,"host");v.buttons[0].enabled=1;strcpy(v.buttons[1].id,"join");v.buttons[1].enabled=1;strcpy(v.buttons[2].id,"start");v.buttons[2].enabled=1;strcpy(v.buttons[3].id,"leave");v.buttons[3].enabled=1;mod_ui_publish(&v);}
static int hook(unsigned id,uint64_t caller,uint64_t x0,uint64_t x1,uint64_t *result){Cpu c={0};c.x[0]=x0;c.x[1]=x1;c.x[30]=caller;c.x[0]=x0;int r=rt_native_ui_hook(id,&c);if(result)*result=c.x[0];return r;}
static int online_input(unsigned selected,uint64_t caller,uint64_t *result){
 Cpu c={0};ST32((uint64_t)view_page+0x90,selected);c.x[19]=(uint64_t)view_page;c.x[30]=caller;int r=rt_native_ui_hook(1,&c);*result=c.x[0];return r;
}
static int state_hook(uint64_t caller){Cpu c={0};c.x[0]=(uint64_t)view_page;c.x[30]=caller;return rt_native_ui_hook(5,&c);}
static unsigned view_state(void){return LD8((uint64_t)view_page+0x94);}
// The failure window's message post: the hook never answers, it may only rewrite the id at +8 of the argument.
static unsigned post_message(uint64_t caller,unsigned id){uint64_t arg=(uint64_t)view_page+0x200;ST16(arg+8,id);Cpu c={0};c.sp=arg-0x348;c.x[1]=arg;c.x[30]=caller;assert(!rt_native_ui_hook(6,&c));return LD16(arg+8);} // the argument is a local of the caller's frame
static unsigned confirms; // accepted Confirms, each queues one native intent and wakes the worker once
static int confirm_option(unsigned state){ST8((uint64_t)view_page+0x94,state);dialog_answer=1;ModIntent i;int ok=state_hook(0x7000000000ull+0x71631c)&&view_state()==(state==0xb?0xcu:0xfu)&&mod_ui_intent_pop(&i);if(ok)confirms++;return ok;}
// The Join flow with the passcode keyboard (a mod that registered `details`, the squad screen parent object readable): the dialog is answered, the
// game's keyboard is requested (state 0x12), it is still open for one poll, then the player has typed `typed` and confirmed; the mod is asked to join.
static int join_confirm(const char *typed){
 ST8((uint64_t)view_page+0x94,0xe);dialog_answer=1;unsigned asked=kbd_requests;
 if(!(state_hook(0x7000000000ull+0x71631c)&&view_state()==0x12&&kbd_requests==asked+1))return 0;
 uint64_t parent=LD64((uint64_t)view_page+0x88);
 ST8(parent+0x182,0);if(!state_hook(0x7000000000ull+0x71631c)||view_state()!=0x12)return 0; // the keyboard is open
 strcpy((char *)(uintptr_t)(parent+0x184),typed);ST8(parent+0x182,1);
 ModIntent i;int ok=state_hook(0x7000000000ull+0x71631c)&&view_state()==0xf&&mod_ui_intent_pop(&i)&&!strcmp(i.action,"join")&&!strcmp(i.text,typed);if(ok)confirms++;return ok;
}
// Confirms the option, reports a failure with the text (NULL: none) and returns the id the failure window ends up posting.
static unsigned fail_with(unsigned state,const char *text){assert(confirm_option(state));assert(mod_native_ui_report_message("alpha",MOD_NATIVE_FAILED,text));assert(state_hook(0x7000000000ull+0x71631c)&&view_state()==0x17);return post_message(0x7000000000ull+0x716cb8,0x12f5);}
static int input(unsigned selected,uint32_t buttons,uint64_t caller){
 Cpu c={0};ST32((uint64_t)view_page+0x90,selected);pad=buttons;c.x[0]=(uint64_t)view_page;c.x[30]=caller;return rt_native_ui_hook(4,&c);
}
#define B 0x7000000000ull
int main(void){
 uint64_t r;(void)animated_component;
 rt_native_ui_busy_min_ns=0; // the minimum time of the busy window is tested on its own below
 rt_native_ui_read64=fake_guest_word; // the dialog holder word lives at a guest address the fixture does not have
 rt_native_ui_next_region=fake_region;stock_table();
 mod_ui_reset("");mod_native_ui_reset();mod_native_ui_set_wake(wake);
 assert(rt_native_ui_capable());
 // No registration: every hook declines and nothing reads the pad.
 assert(!hook(0,B+0x8383e8,0x2280,0,&r)&&!hook(1,B+0x78b8e4,0,0,&r)&&!hook(2,B+0x78bab4,0,0,&r)&&!hook(3,B+0x71632c,0,0,&r));
 assert(!input(2,4,B+0x716338)&&!pad_reads);
 ModNativeUi request={0};strcpy(request.owner,"alpha");request.features=MOD_NATIVE_ONLINE_ENTRY|MOD_NATIVE_SQUAD_SELECT|MOD_NATIVE_DETAILS;
 strcpy(request.action[MOD_NATIVE_CREATE_PRIVATE],"host");strcpy(request.action[MOD_NATIVE_PLAY_PRIVATE],"join");strcpy(request.action[MOD_NATIVE_DETAILS_START],"start");strcpy(request.action[MOD_NATIVE_DETAILS_LEAVE],"leave");
 assert(!strcmp(mod_native_ui_register(&request),"unsupported")); // support never switched on
 mod_native_ui_support(1);
 assert(!mod_native_ui_register(&request)&&!strcmp(mod_native_ui_register(&request),"full"));
 assert(mod_native_ui_features()==(MOD_NATIVE_ONLINE_ENTRY|MOD_NATIVE_SQUAD_SELECT|MOD_NATIVE_DETAILS));
 // Story condition: only 0x2280 with x1 == 0, only from connect entry's enabled check.
 assert(hook(0,B+0x8383e8,0x2280,0,&r)&&r==1);
 assert(!hook(0,B+0x8383ec,0x2280,0,&r)&&!hook(0,B+0x8383e8,0x2281,0,&r)&&!hook(0,B+0x8383e8,0x2280,1,&r));
 // The three connection checks answer yes only from their own call sites.
 struct {unsigned id;uint64_t site;} sites[]={{1,0x78b8e4},{2,0x78bab4},{3,0x71632c}};
 for(unsigned i=0;i<3;++i){r=7;assert(hook(sites[i].id,B+sites[i].site,0,0,&r)&&r==1);assert(!hook(sites[i].id,B+sites[i].site+4,0,0,&r)&&!hook(sites[i].id,B,0,0,&r));}
 assert(!hook(9,B+0x78b8e4,0,0,&r)); // unknown hook id
 // Input: not the registered caller, no Confirm, and a bad view are all left to the original handler.
 publish(MOD_LOADED);
 assert(!input(2,4,B+0x716330));
 assert(!input(2,0,B+0x716338)&&!input(2,0x2000,B+0x716338));
 { Cpu c={0};c.x[0]=0;c.x[30]=B+0x716338;pad=4;assert(!rt_native_ui_hook(4,&c)); }
 ModIntent intent;
 // Create and join card with an action: the original handler runs (it raises the game's dialog); the online query is
 // answered yes for exactly those two options, and only from the input handler's call site.
 animations=0;assert(!input(2,4,B+0x716338)&&!input(3,4,B+0x716338)&&!animations&&!mod_ui_intent_pop(&intent)&&!wakes);
 for(unsigned o=0;o<8;++o){r=7;int yes=online_input(o,B+0x7174a4,&r);assert(yes==(o==2||o==3));if(yes)assert(r==1);}
 assert(!online_input(2,B+0x7174a8,&r)&&!online_input(2,B+0x7174a0,&r));
 { Cpu c={0};c.x[19]=0;c.x[30]=B+0x7174a4;assert(!rt_native_ui_hook(1,&c)); } // bad view
 // The state handler: only from its call site, and only in the states it owns.
 ST8((uint64_t)view_page+0x94,0);assert(!state_hook(B+0x71631c)&&!state_hook(B+0x716320));
 ST8((uint64_t)view_page+0x94,0xb);assert(!state_hook(B+0x716320));
 dialog_answer=0;assert(state_hook(B+0x71631c)&&view_state()==0xb&&!answer_consumed&&!mod_ui_intent_pop(&intent)); // dialog still open
 dialog_answer=-1;assert(state_hook(B+0x71631c)&&view_state()==0&&answer_consumed==1&&!mod_ui_intent_pop(&intent)); // Cancel
 ST8((uint64_t)view_page+0x94,0xb);dialog_answer=2;assert(state_hook(B+0x71631c)&&view_state()==0&&answer_consumed==2); // Back
 ST8((uint64_t)view_page+0x94,0xb);dialog_answer=1; // Confirm: the mod action runs and the view waits
 assert(state_hook(B+0x71631c)&&view_state()==0xc&&answer_consumed==3&&mod_ui_intent_pop(&intent)&&intent.native&&!strcmp(intent.action,"host"));
 assert(mod_ui_intent_current(&intent));
 // Confirming found the spare message rows in the (fake) heap without changing them; the string after them is intact.
 for(unsigned k=0;k<MESSAGE_ROWS;++k)assert(row_says(k,NULL));
 assert(!strcmp((char *)(uintptr_t)row_pos[MESSAGE_ROWS-1]+message_text_row(MESSAGE_ROWS-1)->length+1,"neighbour"));
 assert(region_calls>0);unsigned searched=region_calls;confirms=1; // the next Confirm finds live copies and does not search again
 assert(state_hook(B+0x71631c)&&view_state()==0xc); // no report yet: keeps waiting
 assert(mod_native_ui_report("alpha",MOD_NATIVE_OK)&&!mod_native_ui_report("beta",MOD_NATIVE_OK)&&!mod_native_ui_report("alpha",7));
 assert(state_hook(B+0x71631c)&&view_state()==0); // success: back to idle
 // The Details screen: a reported success opens it (the mod's passcode and mode, or the mode the option implies and a placeholder)
 // after the game's busy window, hides the squad screen with "out", and three hooks stand in for the online session only
 // while it is open, each at its one guarded call site. Closing it brings the squad screen back with "in" and asks the mod.
 ST64((uint64_t)view_page+0x88,(uint64_t)view_page+0x300);ST8((uint64_t)view_page+0xb2,1);
 assert(!hook(7,B+0x715c84,0,0,&r)&&!hook(8,B+0x715ca4,0,0,&r)); // nothing is open yet
 busy_ons=busy_offs=outs=ins=0;
 assert(confirm_option(0xb)&&busy_ons==1&&busy_offs==0&&busy_object==0x7777); // the wait shows the game's busy window
 assert(mod_native_ui_report("alpha",MOD_NATIVE_OK)&&state_hook(B+0x71631c)&&view_state()==0);
 assert(busy_offs==1&&busy_object==0x7777&&outs==1&&ins==0&&LD8((uint64_t)view_page+0xb2)==0); // window off, squad screen hidden
 assert(detail_opens==1&&detail_mode==0&&!strcmp(detail_code,"000000")&&detail_parent==(uint64_t)view_page+0x300&&detail_clicks==1);
 assert(hook(7,B+0x715c84,0,0,&r)&&r==1&&!hook(7,B+0x715c88,0,0,&r)); // the "still online?" query: yes, at its call site only
 assert(hook(8,B+0x715ca4,0,0,&r)&&!hook(8,B+0x715ca8,0,0,&r)&&status_writes==0);     // the countdown refresh is skipped, at its call site only; no status from the mod: no text written
 { unsigned reads=pad_reads;assert(input(2,4,B+0x716338)&&pad_reads==reads&&!mod_ui_intent_pop(&intent)); } // Squad screen takes no input under it
 { Cpu c={0};c.x[19]=(uint64_t)view_page;c.x[30]=B+0x715d78;ST32((uint64_t)view_page+0x70,1);fake_status=2;unsigned before=status_sets;
   assert(!rt_native_ui_hook(9,&c)&&status_sets==before+1&&status_manager==0x9999&&last_status==0); // closed by button 1: the status is cleared, the close runs
   assert(ins==1&&LD8((uint64_t)view_page+0xb2)==1);                                           // Squad screen is back
   assert(mod_ui_intent_pop(&intent)&&intent.native&&!strcmp(intent.action,"leave"));confirms++;           // Leave button asks the mod to leave
   assert(hook(7,B+0x715c84,0,0,&r)&&r==1&&hook(8,B+0x715ca4,(uint64_t)view_page,0,&r));  // still fading out: the hooks keep answering
   assert(!input(2,0,B+0x716338));                                                                           // Squad screen runs again
   assert(!hook(7,B+0x715c84,0,0,&r)&&!hook(8,B+0x715ca4,0,0,&r)); }
 // The mod's own passcode, and its mode for a screen opened again while a session is up ("in squad": the return button only closes).
 assert(join_confirm("27015")&&mod_native_ui_report_details("alpha","27015",MOD_NATIVE_MODE_NONE,"Players:","2 / 4")&&state_hook(B+0x71631c)&&view_state()==0&&detail_opens==2&&detail_mode==1&&!strcmp(detail_code,"27015"));
 // Start button (button 0) leaves the menu the way the original does: the status stays 1 (also on the object state 0x11 reads), Squad
 // Select is not shown again, the view goes to state 0x11 so the game's own code exits, and the mod is asked to start. The screen
 // is then opened again for the checks that follow.
 { Cpu c={0};c.x[19]=(uint64_t)view_page;c.x[30]=B+0x715d78;ST32((uint64_t)view_page+0x70,0);unsigned sets=status_sets,shown=ins;fake_status=1;
   assert(!rt_native_ui_hook(9,&c)&&status_sets==sets+1&&status_manager==0x1234&&last_status==1&&ins==shown&&view_state()==0x11);
   assert(mod_ui_intent_pop(&intent)&&intent.native&&!strcmp(intent.action,"start"));confirms++;ST8((uint64_t)view_page+0x94,0); }
 assert(join_confirm("27015")&&mod_native_ui_report_details("alpha","27015",MOD_NATIVE_MODE_NONE,"Players:","2 / 4")&&state_hook(B+0x71631c)&&view_state()==0&&detail_opens==3);
 // The timer slot: the mod's label replaces the timer label while the screen is open, and its status is written into the countdown pane.
 assert(row_says(MESSAGE_LABEL_ROW,"Players:"));
 assert(hook(8,B+0x715ca4,(uint64_t)view_page,0,&r)&&status_writes==1&&!strcmp(status_text,"2 / 4")&&pane_view==(uint64_t)view_page+0x28&&pane_name==B+0x1784196&&setter_arg==0x4242);
 assert(mod_native_ui_details_status("alpha","3 / 4")&&hook(8,B+0x715ca4,(uint64_t)view_page,0,&r)&&status_writes==2&&!strcmp(status_text,"3 / 4")); // changed while open
 assert(!mod_native_ui_details_status("beta","1 / 4")&&!mod_native_ui_details_status("alpha","")&&!mod_native_ui_details_status("alpha","123456789")&&!mod_native_ui_details_status("alpha","bad\x01"));
 assert(!mod_native_ui_report_details("alpha","27015",MOD_NATIVE_MODE_NONE,"Fourteen chars",NULL)&&!mod_native_ui_report_details("alpha","27015",MOD_NATIVE_MODE_NONE,NULL,"123456789"));
 { Cpu c={0};c.x[19]=(uint64_t)view_page;c.x[30]=B+0x715d78;ST32((uint64_t)view_page+0x70,0);assert(!rt_native_ui_hook(9,&c)&&mod_ui_intent_pop(&intent)&&!strcmp(intent.action,"start"));confirms++;assert(row_says(MESSAGE_LABEL_ROW,NULL)); } // Start button asks the mod to start
 assert(confirm_option(0xb)&&mod_native_ui_report_details("alpha","27015",MOD_NATIVE_MODE_IN_SQUAD,NULL,NULL)&&state_hook(B+0x71631c)&&detail_opens==4&&detail_mode==2);
 { Cpu c={0};c.x[19]=(uint64_t)view_page;c.x[30]=B+0x715d78;ST32((uint64_t)view_page+0x70,0);assert(!rt_native_ui_hook(9,&c)&&!mod_ui_intent_pop(&intent)); } // Return button: no action
 assert(!mod_native_ui_report_details("beta","27015",MOD_NATIVE_MODE_NONE,NULL,NULL)&&!mod_native_ui_report_details("alpha","bad code",MOD_NATIVE_MODE_NONE,NULL,NULL)&&!mod_native_ui_report_details("alpha","0123456789abcdef",MOD_NATIVE_MODE_NONE,NULL,NULL)&&!mod_native_ui_report_details("alpha","1",7,NULL,NULL)); // owner, characters, length, mode
 // The passcode keyboard: Cancel returns to idle and asks nothing; an empty or odd text shows the game's own squad-not-found window.
 { uint64_t parent=LD64((uint64_t)view_page+0x88);unsigned asked=kbd_requests;
   ST8((uint64_t)view_page+0x94,0xe);dialog_answer=1;assert(state_hook(B+0x71631c)&&view_state()==0x12&&kbd_requests==asked+1&&kbd_parent==parent);
   ST8(parent+0x182,2);assert(state_hook(B+0x71631c)&&view_state()==0&&!mod_ui_intent_pop(&intent)); // cancelled
   for(unsigned k=0;k<3;++k){const char *bad[3]={"","bad code","semi;colon"};
     ST8((uint64_t)view_page+0x94,0xe);dialog_answer=1;assert(state_hook(B+0x71631c)&&view_state()==0x12);
     ST8(parent+0x182,0);strcpy((char *)(uintptr_t)(parent+0x184),bad[k]);ST8(parent+0x182,1);
     unsigned shown=busy_ons;assert(state_hook(B+0x71631c)&&view_state()==0xf&&!mod_ui_intent_pop(&intent)&&busy_ons==shown+1);   // the busy window comes first
     assert(state_hook(B+0x71631c)&&view_state()==0x17&&post_message(B+0x716cb8,0x12f5)==0x156c);}                                // then the game's own squad-not-found text
   ST8((uint64_t)view_page+0x94,0xe);dialog_answer=1;assert(state_hook(B+0x71631c)&&view_state()==0x12);                 // a passcode of 16 characters is cut to 15
   ST8(parent+0x182,0);strcpy((char *)(uintptr_t)(parent+0x184),"0123456789abcdefg");ST8(parent+0x182,1);
   assert(state_hook(B+0x71631c)&&view_state()==0xf&&mod_ui_intent_pop(&intent)&&!strcmp(intent.text,"0123456789abcde"));confirms++;
   assert(mod_native_ui_report("alpha",MOD_NATIVE_FAILED)&&state_hook(B+0x71631c)&&view_state()==0x17&&post_message(B+0x716cb8,0x12f5)==0x156c); // the join ends: the busy window goes
   ST8((uint64_t)view_page+0x94,0); }
 // While the mod says a session is up, join card skips the keyboard and asks the mod at once (no text).
 assert(mod_native_ui_set_session("alpha",1)&&!mod_native_ui_set_session("beta",1)&&mod_native_ui_session_active());
 { unsigned asked=kbd_requests;ST8((uint64_t)view_page+0x94,0xe);dialog_answer=1;ModIntent i;
   assert(state_hook(B+0x71631c)&&view_state()==0xf&&kbd_requests==asked&&mod_ui_intent_pop(&i)&&!strcmp(i.action,"join")&&!i.text[0]);confirms++;
   assert(mod_native_ui_report("alpha",MOD_NATIVE_FAILED)&&state_hook(B+0x71631c)&&view_state()==0x17&&post_message(B+0x716cb8,0x12f5)==0x156c);ST8((uint64_t)view_page+0x94,0); }
 // And the game's online check answers yes to any other caller (the player-list row and the like), but never to the squad screen option
 // handler's own call, which keeps its dialog-option rule so first card is not sent into the missing online client.
 assert(hook(1,B+0x6afef4+4,0,0,&r)&&r==1&&hook(1,B+0x4b1c10+8,0,0,&r)&&r==1);
 assert(!online_input(0,B+0x7174a4,&r)&&!online_input(1,B+0x7174a4,&r)&&online_input(2,B+0x7174a4,&r)&&r==1);
 assert(hook(7,B+0x6aef00,0,0,&r)&&r==1&&hook(7,B+0x6aff00,0,0,&r)&&r==1); // the session query too: the Online menu, the player-list row
 assert(hook(10,B+0x838448,0,0,&r)&&r==1&&hook(10,B+0x838560,0,0,&r)&&r==1); // and the "online state ready" query of the menu rows' validators
 assert(mod_native_ui_set_session("alpha",0)&&!mod_native_ui_session_active()&&!hook(10,B+0x838448,0,0,&r)); // never without a session
 assert(!hook(1,B+0x6afef4+4,0,0,&r)); // no session: only the connect state machine's own call site is answered
 assert(!hook(7,B+0x6aef00,0,0,&r)); // ... and the session query only at the Details screen's own call site, while that screen is open
 // The busy window stays up for its minimum time even when the mod answers at once.
 rt_native_ui_busy_min_ns=700000000ull;busy_ons=busy_offs=0;
 assert(join_confirm("27015")&&busy_ons==1&&mod_native_ui_report("alpha",MOD_NATIVE_OK));
 assert(state_hook(B+0x71631c)&&view_state()==0xf&&busy_offs==0); // answered, still showing the window
 fake_now+=699999999ull;assert(state_hook(B+0x71631c)&&view_state()==0xf&&busy_offs==0);
 fake_now+=1;assert(state_hook(B+0x71631c)&&view_state()==0&&busy_offs==1);   // long enough: the window goes and the screen proceeds
 hook(2,B+0x78bab4,0,0,&r);rt_native_ui_busy_min_ns=0;                          // (a new view clears the Details screen that just opened)
 // A failure and a wait that runs out both end the busy window before the failure message.
 busy_offs=0;assert(confirm_option(0xb)&&mod_native_ui_report("alpha",MOD_NATIVE_FAILED)&&state_hook(B+0x71631c)&&view_state()==0x17&&busy_offs==1);
 assert(post_message(B+0x716cb8,0x12f5)==0x12f5);
 // A new squad screen view is built: the old Details screen is gone.
 assert(join_confirm("27015")&&mod_native_ui_report("alpha",MOD_NATIVE_OK)&&state_hook(B+0x71631c)&&hook(7,B+0x715c84,0,0,&r)&&r==1);
 assert(hook(2,B+0x78bab4,0,0,&r)&&r==1);assert(!hook(7,B+0x715c84,0,0,&r));
 { Cpu c={0};c.x[19]=(uint64_t)view_page;c.x[30]=B+0x715d78;unsigned sets=status_sets;assert(!rt_native_ui_hook(9,&c)&&status_sets==sets&&!mod_ui_intent_pop(&intent)); } // a close with nothing open does nothing
 ST64((uint64_t)view_page+0x88,0); // later flows run without a parent object, as before
 // A failure without text shows the game's own window: Host create-failed (0x12f5, unchanged), Join 0x156c.
 assert(confirm_option(0xb)&&mod_native_ui_report("alpha",MOD_NATIVE_FAILED)&&state_hook(B+0x71631c)&&view_state()==0x17);
 assert(post_message(B+0x716cb8,0x12f5)==0x12f5);
 assert(confirm_option(0xe)&&mod_native_ui_report("alpha",MOD_NATIVE_FAILED)&&state_hook(B+0x71631c)&&view_state()==0x17);
 assert(post_message(B+0x716cbc,0x12f5)==0x12f5&&post_message(B+0x716cb8,0x12f5)==0x156c&&post_message(B+0x716cb8,0x12f5)==0x12f5); // once, only at the guarded post
 // A failure with the mod's own text shows it in a spare row, in the same window.
 assert(fail_with(0xe,"First test text.")==message_text_row(0)->id&&row_says(0,"First test text."));
 assert(fail_with(0xb,"First test text.")==message_text_row(0)->id); // the same text reuses its row, for either option
 assert(fail_with(0xb,"Second test text.")==message_text_row(1)->id&&row_says(1,"Second test text."));
 assert(fail_with(0xe,"Third test text.")==message_text_row(2)->id&&row_says(2,"Third test text."));
 assert(fail_with(0xb,"Line one\nLine two")==message_text_row(0)->id&&row_says(0,"Line one\nLine two")); // the fourth takes the least recently used row
 assert(fail_with(0xb,"First test text.")==message_text_row(1)->id&&row_says(1,"First test text.")&&row_says(2,"Third test text."));
 assert(!strcmp((char *)(uintptr_t)row_pos[MESSAGE_ROWS-1]+message_text_row(MESSAGE_ROWS-1)->length+1,"neighbour"));
 assert(region_calls==searched); // every Confirm so far found live copies: no further search
 // The registry refuses a text that is empty, too long, not printable, with ok, or from another mod; a refused report changes nothing.
 {char longer[MESSAGE_TEXT_MAX+2];memset(longer,'a',sizeof longer);longer[sizeof longer-1]=0;char exact[MESSAGE_TEXT_MAX+1];memset(exact,'b',sizeof exact);exact[sizeof exact-1]=0;
  assert(confirm_option(0xb));
  assert(!mod_native_ui_report_message("alpha",MOD_NATIVE_FAILED,"")&&!mod_native_ui_report_message("alpha",MOD_NATIVE_FAILED,longer)&&!mod_native_ui_report_message("alpha",MOD_NATIVE_FAILED,"bad\x01text")&&!mod_native_ui_report_message("alpha",MOD_NATIVE_FAILED,"caf\xc3\xa9"));
  assert(!mod_native_ui_report_message("alpha",MOD_NATIVE_OK,"text")&&!mod_native_ui_report_message("beta",MOD_NATIVE_FAILED,"text")&&mod_native_ui_result()==MOD_NATIVE_PENDING);
  assert(mod_native_ui_report_message("alpha",MOD_NATIVE_FAILED,exact)&&state_hook(B+0x71631c)&&view_state()==0x17&&post_message(B+0x716cb8,0x12f5)<=message_text_row(2)->id); // the longest text fits
  assert(row_says(0,exact)||row_says(1,exact)||row_says(2,exact));}
 // The toast: the owner asks, the state hook (any state) takes the text once, writes it into the toast row, requests the game's
 // toast with id 0x241 on the message manager, and puts the row back later. One toast at a time; the latest text waits.
 {char t47[MESSAGE_TOAST_MAX+2];memset(t47,'x',sizeof t47);t47[sizeof t47-1]=0;char t46[MESSAGE_TOAST_MAX+1];memset(t46,'y',sizeof t46);t46[sizeof t46-1]=0;
  ST8((uint64_t)view_page+0x94,0);
  assert(!mod_native_ui_toast("beta","Hosting...")&&!mod_native_ui_toast("alpha","")&&!mod_native_ui_toast("alpha","two\nlines")&&!mod_native_ui_toast("alpha","caf\xc3\xa9")&&!mod_native_ui_toast("alpha",t47));
  assert(!state_hook(B+0x71631c)&&!toast_posts&&row_says(MESSAGE_TOAST_ROW,NULL)); // nothing asked: nothing happens
  assert(mod_native_ui_toast("alpha","Starting a squad..."));
  assert(!state_hook(B+0x71631c)&&toast_posts==1&&toast_manager==0x9999&&toast_id==0x241&&row_says(MESSAGE_TOAST_ROW,"Starting a squad..."));
  assert(!strcmp((char *)(uintptr_t)row_pos[MESSAGE_ROWS-1]+message_text_row(MESSAGE_ROWS-1)->length+1,"neighbour")); // the neighbour is intact
  assert(mod_native_ui_toast("alpha","Second")&&mod_native_ui_toast("alpha","Third")); // waits for the one on screen; the latest wins
  fake_now+=TOAST_NS-1;assert(!state_hook(B+0x71631c)&&toast_posts==1&&row_says(MESSAGE_TOAST_ROW,"Starting a squad...")); // just short of the hold: still there
  fake_now+=1;assert(!state_hook(B+0x71631c)&&toast_posts==2&&row_says(MESSAGE_TOAST_ROW,"Third")); // put back, and the waiting text takes over
  // Idle (state 0) never reaches the state hook in the game (owner run 5): the idle hook expires and shows toasts on its own.
  {uint64_t r=0;fake_now+=TOAST_NS-1;assert(hook(3,B+0x71632c,0,0,&r)&&r==1&&toast_posts==2&&row_says(MESSAGE_TOAST_ROW,"Third"));
   fake_now+=1;assert(hook(3,B+0x71632c,0,0,&r)&&r==1&&toast_posts==2&&row_says(MESSAGE_TOAST_ROW,NULL)); // restored from idle alone
   assert(mod_native_ui_toast("alpha","From idle")&&hook(3,B+0x71632c,0,0,&r)&&r==1&&toast_posts==3&&row_says(MESSAGE_TOAST_ROW,"From idle"));}
  // The restore needs neither the state nor the right caller: any later call of a squad screen hook puts the row back.
  fake_now+=TOAST_NS;assert(!state_hook(B+0x999)&&row_says(MESSAGE_TOAST_ROW,NULL)&&toast_posts==3);
  ST8((uint64_t)view_page+0x94,0xc);assert(!state_hook(B+0x71631c)); // a state the toast does not care about is untouched
  ST8((uint64_t)view_page+0x94,0);
  assert(mod_native_ui_toast("alpha",t46)&&!state_hook(B+0x71631c)&&toast_posts==4&&row_says(MESSAGE_TOAST_ROW,t46)); // the longest text fits the row
  fake_now+=TOAST_NS;assert(!state_hook(B+0x71631c));
  assert(row_says(MESSAGE_TOAST_ROW,NULL));
  for(unsigned k=0;k<MESSAGE_POOL_ROWS;++k)assert(row_says(k,NULL)||k<MESSAGE_POOL_ROWS); // the pool rows are untouched by toasts
 }
 // Rows that cannot be found (another language, or a modified table) fall back to the game's own text: 0x156c for Join, unchanged for Host.
 memset(fake_heap,0,sizeof fake_heap);unsigned before=region_calls;
 assert(fail_with(0xe,"First test text.")==0x156c&&fail_with(0xb,"Second test text.")==0x12f5&&region_calls>before);
 {unsigned posts=toast_posts;assert(mod_native_ui_toast("alpha","Hosting...")&&!state_hook(B+0x71631c)&&toast_posts==posts);} // the toast row cannot be found: the request is dropped, nothing is posted
 stock_table();before=region_calls; // the searches are limited: a table that appears later is not searched for again
 assert(fail_with(0xe,"First test text.")==0x156c&&region_calls==before);
 // A post whose argument is not in the caller's frame is left alone (and the replacement is spent).
 assert(confirm_option(0xe)&&mod_native_ui_report("alpha",MOD_NATIVE_FAILED)&&state_hook(B+0x71631c));
 {uint64_t arg=(uint64_t)view_page+0x200;ST16(arg+8,0x12f5);Cpu c={0};c.sp=arg+0x2000;c.x[1]=arg;c.x[30]=B+0x716cb8;assert(!rt_native_ui_hook(6,&c)&&LD16(arg+8)==0x12f5);}
 assert(confirm_option(0xe));
 for(unsigned i=0;i<60*20+2&&view_state()==0xf;++i)assert(state_hook(B+0x71631c)); // no answer: gives up with the game's own failure message
 assert(view_state()==0x17&&post_message(B+0x716cb8,0x12f5)==0x156c);
 ST8((uint64_t)view_page+0x94,0xc);assert(!state_hook(B+0x71631c)); // not waiting for a mod: the original state runs
 ST8((uint64_t)view_page+0x94,0);
 // An owner that faulted at the dialog: Confirm returns the view to idle and queues nothing.
 ST8((uint64_t)view_page+0x94,0xb);dialog_answer=1;publish(MOD_FAULTED);assert(state_hook(B+0x71631c)&&view_state()==0&&!mod_ui_intent_pop(&intent));publish(MOD_LOADED);
 // First card and second card have no action: Confirm is swallowed without animation or intent.
 animations=0;reroutes=unselects=0;
 assert(input(0,4,B+0x716338)&&!animations&&reroutes==1&&unselects==1&&!mod_ui_intent_pop(&intent));assert(!input(0,0,B+0x716338)); // answered by the game's unavailable message, highlight kept
 assert(input(1,4,B+0x716338)&&!animations&&reroutes==2&&unselects==1&&!mod_ui_intent_pop(&intent));assert(!input(1,0,B+0x716338));
 assert(input(7,4,B+0x716338)&&!animations&&!mod_ui_intent_pop(&intent));assert(!input(7,0,B+0x716338)); // corrupt selection
 // An owner that faulted gets nothing, but the original option code still never runs.
 publish(MOD_FAULTED);
 assert(input(2,4,B+0x716338)&&!animations&&!mod_ui_intent_pop(&intent));assert(!input(2,0,B+0x716338));
 publish(MOD_LOADED);
 // The Mods overlay session plays no part: opening and closing it neither queues nor drops a native intent.
 mod_ui_interaction(1);mod_ui_interaction(0);
 assert(!input(2,4,B+0x716338)&&!mod_ui_intent_pop(&intent));assert(!input(2,0,B+0x716338));assert(wakes==confirms); // each accepted intent woke the worker once
 // Shutdown withdraws the registration: hooks decline, intents are refused.
 mod_native_ui_invalidate();
 assert(!mod_native_ui_features()&&!hook(1,B+0x78b8e4,0,0,&r)&&!input(2,4,B+0x716338)&&!state_hook(B+0x71631c)&&!online_input(2,B+0x7174a4,&r));assert(!mod_native_ui_confirm(2)&&!mod_native_ui_has_action(2)&&!mod_native_ui_report("alpha",MOD_NATIVE_OK)&&!mod_native_ui_toast("alpha","Hello"));
 mod_native_ui_reset();assert(!strcmp(mod_native_ui_register(&request),"unsupported"));
 // A native intent from a stopped host or an unknown action is refused.
 mod_native_ui_support(1);assert(!mod_native_ui_register(&request));assert(!mod_ui_native_intent_submit("alpha","missing")&&!mod_ui_native_intent_submit("beta","host"));
 mod_ui_shutdown();assert(!mod_ui_native_intent_submit("alpha","host"));
 puts("native ui adapter: callers, story/online/flag/idle answers, confirm edge, animation, inert options, owner fault, withdrawal");
}
