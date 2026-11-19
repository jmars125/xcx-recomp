#include "mods_page.h"
#include "imgui.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
extern "C" {
#include "mods.h"
#include "mod_settings.h"
#include "mod_library.h"
#include "romfs_mods.h"
#include "bdat_merge.h"
#include "ui_text.h"
// host.h is C only: the one the page needs, as declared there
int rt_restart_game(void);
int rt_restart_started(void);
}
#ifdef XCX_MOD_UI_TESTING
#include "imgui_internal.h"
struct ModsWidget {ImGuiID id;ImGuiWindow *window;ImVec2 low,high;char mod[65],field[65];};
static ModsWidget mods_widgets[128];static int mods_widget_count;
static ImVec2 mods_row_low[MOD_UI_MAX],mods_row_high[MOD_UI_MAX];
static ImGuiID mods_button_id[4];static ImGuiWindow *mods_button_window[4];
static ImVec2 mods_button_low[4],mods_button_high[4];static int mods_button_count;
static ImVec2 mods_retry_low,mods_retry_high;static bool mods_retry_visible;
// the Mod priority section as drawn (tests/mods_priority_check.cpp): each line and how it looks (P a priority row,
// H the merges' heading, R the restart line, O left out, Y caveat, G grey, N a summary; M the Merge now button, W
// merging, X the next start's heading, then its lines as above, Z none to merge, S the start-up lines' heading, A the
// merge-automatically box, D a mod's own lines under its row, E none for it, U Revert to the start-up list, Q Restart
// now), and the rows' Up buttons
static char prio_lines[64][340],prio_tag[64];static int prio_count;static ImVec2 prio_up_low[32],prio_up_high[32];
static ImVec2 prio_off_low[32],prio_off_high[32];static bool prio_off_shown[32];   // Turn off buttons; T lines: tags
static ImVec2 prio_merge_low,prio_merge_high,prio_auto_low,prio_auto_high,prio_undo_low,prio_undo_high,prio_restart_low,prio_restart_high;
static ImVec2 prio_tag_low[32],prio_tag_high[32];   // the rows' notes, which open their lines (D)
static ImVec2 prio_yes_low,prio_yes_high;            // the restart confirmation's Restart (C: its question)
static void prio_line(char tag,const char *text){if(prio_count<64){prio_tag[prio_count]=tag;snprintf(prio_lines[prio_count++],sizeof prio_lines[0],"%s",text);}}
#define PRIO_LINE(tag,text) prio_line(tag,text)
#else
#define PRIO_LINE(tag,text) ((void)0)
#endif
static const char *status_name(int status) {
    switch(status) {case MOD_LOADED:return "Loaded";case MOD_DISABLED:return "Disabled";case MOD_INCOMPATIBLE:return "Incompatible";case MOD_FAULTED:return "Faulted";default:return "Starting";}
}
static char requested_owner[65],requested_page[65];
void mods_page_select(const char *owner,const char *page){snprintf(requested_owner,sizeof requested_owner,"%s",owner);snprintf(requested_page,sizeof requested_page,"%s",page);}

// A merge's lines (this run's, or with ahead those made for the next start): the summaries plain, changes left out
// orange, caveats yellow, mods that add nothing or are partly alike grey. The count drawn.
static void merge_line(int kind,const char *line) {
    if(kind>=3){ImGui::TextDisabled("%s",line);return;}   // a mod that adds nothing, or partly alike: no conflict
    if(kind)ImGui::PushStyleColor(ImGuiCol_Text,kind==1?ImVec4(1.0f,0.7f,0.4f,1):ImVec4(1.0f,0.9f,0.5f,1));
    ImGui::TextUnformatted(line);
    if(kind)ImGui::PopStyleColor();
}

static int merge_lines(bool ahead) {
    char line[320];int kind=0,i=0;
    for(;ahead?bdat_merge_next_status(i,line,sizeof line,&kind):bdat_merge_status(i,line,sizeof line,&kind);++i){
        PRIO_LINE(kind>=3?'G':kind==1?'O':kind==2?'Y':'N',line);
        merge_line(kind,line);
    }
    return i;
}

// The game-file mods that are on, highest priority first as romfs-enabled.txt lists them (runtime/romfs_mods.c,
// romfs_mods_priority), with Up and Down to change which one wins, and what the data table merges did this run
// (runtime/bdat_merge_serve.c): a summary per file, then its changes left out (orange) and caveats (yellow). Shown
// while any is on, or when a merge ran. The list file is read again once a second and after a change.
// Once the list differs from what the game merged at start-up, Merge now merges it ahead (romfs_mods_premerge): its
// lines and tags then show what the next start will have, and that start uses the files it made. "Merge
// automatically" (bdat_merge_auto) has romfs_mods.c do that by itself after each change. A row's note opens that
// mod's own lines from the merge reports under it; Revert to the start-up list undoes the changes, and Restart now
// restarts the game (asking first in the world: anything since the last save would be lost).
static void mod_priority(const ModLibView &v,bool idle) {
    static char ids[32][65],tag[32][200];static int n,tag_flags[32],changed[32];
    static uint64_t seen=~0ull;static double next;static bool stale,listchg,restart_failed;static int ahead;
    static char open_id[65],dl[64][320];static int dk[64],dn;   // the row whose lines are open, and its lines
    double now=ImGui::GetTime();
    // a new library view at once while it is idle; during a download or install (which publishes one per chunk) once
    // a second, like the timer: each refresh looks at every mod file of the merged tables (romfs_mods_check_files)
    if((v.generation!=seen&&idle)||now>=next){
        romfs_mods_check_files();   // one look at the files for all rows; the calls below compare what it saw
        listchg=romfs_mods_list_changed()!=0;
        n=romfs_mods_priority(ids,32);stale=romfs_mods_tables_changed()!=0;seen=v.generation;next=now+1.0;
        ahead=stale?romfs_mods_premerge_state():0;
        if(ahead==1||(stale&&!ahead&&romfs_mods_auto_merge()))next=now+0.25;   // merging, or about to: look again soon
        for(int i=0;i<n;++i){
            changed[i]=stale?romfs_mod_changed(ids[i]):0;
            bool got=stale&&ahead==2?bdat_merge_next_mod_tag(ids[i],tag[i],sizeof tag[i],&tag_flags[i])
                                    :bdat_merge_mod_tag(ids[i],tag[i],sizeof tag[i],&tag_flags[i]);
            if(!got)tag[i][0]=0;
        }
        if(open_id[0])dn=bdat_merge_mod_lines(open_id,stale&&ahead==2,dl,dk,64);
    }
    bool tags_now=!stale||ahead==2;   // tags of the list as it is: this run's, or those merged ahead for it
    char line[320];int kind=0;bool merges=bdat_merge_status(0,line,sizeof line,&kind)!=0;
    if(n<1&&!merges&&!stale)return;   // one mod on stays listed, for its Turn off
    ImGui::Spacing();ImGui::SeparatorText(menu_text(UI_MODS_PRIORITY));
    bool autom=romfs_mods_auto_merge()!=0;
    if(ImGui::Checkbox(menu_text(UI_MODS_MERGE_AUTO),&autom)){romfs_mods_set_auto_merge(autom);next=0;}
    PRIO_LINE('A',menu_text(UI_MODS_MERGE_AUTO));
#ifdef XCX_MOD_UI_TESTING
    prio_auto_low=ImGui::GetItemRectMin();prio_auto_high=ImGui::GetItemRectMax();
#endif
    if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",menu_text(UI_MODS_MERGE_AUTO_DESC));
    if(n>=1){
        ImGui::PushTextWrapPos(0);ImGui::TextDisabled("%s",menu_text(UI_MODS_PRIORITY_DESC));ImGui::PopTextWrapPos();
        int move=-1,off=-1;bool up=false;
        for(int i=0;i<n;++i){
            const char *name=ids[i];
            for(int k=0;k<v.count;++k)if(!strcmp(v.rows[k].id,ids[i])&&v.rows[k].name[0])name=v.rows[k].name;
            ImGui::PushID(ids[i]);
            ImGui::BeginDisabled(!idle||i==0);if(ImGui::Button(menu_text(UI_MODS_UP))){move=i;up=true;}ImGui::EndDisabled();
#ifdef XCX_MOD_UI_TESTING
            if(i<32){prio_up_low[i]=ImGui::GetItemRectMin();prio_up_high[i]=ImGui::GetItemRectMax();}
#endif
            ImGui::SameLine();
            ImGui::BeginDisabled(!idle||i==n-1);if(ImGui::Button(menu_text(UI_MODS_DOWN))){move=i;up=false;}ImGui::EndDisabled();
            ImGui::SameLine();   // every mod can be turned off from here, beside its Up and Down
            ImGui::BeginDisabled(!idle);if(ImGui::Button(menu_text(UI_MODS_TURN_OFF)))off=i;ImGui::EndDisabled();
#ifdef XCX_MOD_UI_TESTING
            if(i<32){prio_off_low[i]=ImGui::GetItemRectMin();prio_off_high[i]=ImGui::GetItemRectMax();prio_off_shown[i]=true;}
#endif
            ImGui::SameLine();ImGui::Text("%d. %s",i+1,name);PRIO_LINE('P',name);
            // the mods behind the restart line: updated, or turned on, since the game read its tables
            if(changed[i]){
                const char *what=menu_text(changed[i]==1?UI_MODS_CHANGED_UPDATED:UI_MODS_CHANGED_ON);
                ImGui::SameLine();ImGui::TextColored(ImVec4(1.0f,0.82f,0.1f,1),"(%s)",what);PRIO_LINE('V',what);
            }
            // what the merges said about it, after its name
            if(tag[i][0]&&tags_now){
                ImGui::SameLine();
                int f=tag_flags[i];
                ImVec4 c=f&BDAT_TAG_LEFT_OUT?ImVec4(1.0f,0.7f,0.4f,1):f&BDAT_TAG_CAVEAT?ImVec4(1.0f,0.9f,0.5f,1):ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                // a button that looks like the note, so a controller reaches it too: it opens this mod's lines below
                char label[220];snprintf(label,sizeof label,"(%s)##tag",tag[i]);
                ImGui::PushStyleColor(ImGuiCol_Text,c);ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0,0,0,0));
                if(ImGui::SmallButton(label)){
                    if(!strcmp(open_id,ids[i]))open_id[0]=0;
                    else{snprintf(open_id,sizeof open_id,"%s",ids[i]);dn=bdat_merge_mod_lines(open_id,stale&&ahead==2,dl,dk,64);}
                }
                ImGui::PopStyleColor(2);PRIO_LINE('T',tag[i]);
#ifdef XCX_MOD_UI_TESTING
                if(i<32){prio_tag_low[i]=ImGui::GetItemRectMin();prio_tag_high[i]=ImGui::GetItemRectMax();}
#endif
                if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",menu_text(UI_MODS_DETAILS_HINT));
                if(!strcmp(open_id,ids[i])){
                    ImGui::Indent();ImGui::PushTextWrapPos(0);
                    if(!dn){ImGui::TextDisabled("%s",menu_text(UI_MODS_DETAILS_NONE));PRIO_LINE('E',menu_text(UI_MODS_DETAILS_NONE));}
                    for(int k=0;k<dn;++k){merge_line(dk[k],dl[k]);PRIO_LINE('D',dl[k]);}
                    ImGui::PopTextWrapPos();ImGui::Unindent();
                }
            }
            ImGui::PopID();
        }
        if(move>=0){romfs_mods_move(ids[move],up);next=0;}
        // through the library, as its On box does, so its row above shows the change at once; a mod it doesn't list
        // (a folder it can't read) straight in the list file
        if(off>=0){
            bool listed=false;
            for(int k=0;k<v.count;++k)listed|=!strcmp(v.rows[k].id,ids[off])&&v.rows[k].installed;
            if(listed)modlib_set_enabled(ids[off],0);else romfs_mods_set(ids[off],0);
            next=0;
        }
    }
    // The merges were made at start-up and the game doesn't read its tables again, so once the list would make them
    // otherwise (romfs_mods_tables_changed) the lines say so first, instead of passing for the list as it is now, and
    // offer to merge the list as it is now ahead of the restart.
    if(merges||stale){
        ImGui::PushTextWrapPos(0);
        if(!stale){
            ImGui::TextDisabled("%s",menu_text(UI_MODS_MERGES));PRIO_LINE('H',menu_text(UI_MODS_MERGES));
            merge_lines(false);
            ImGui::PopTextWrapPos();
            return;
        }
        PRIO_LINE('R',menu_text(UI_MODS_MERGES_STALE));
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.0f,0.82f,0.1f,1));ImGui::TextUnformatted(menu_text(UI_MODS_MERGES_STALE));ImGui::PopStyleColor();
        // the buttons right under it, in a row: Merge now (unless merging by itself), Revert to the start-up list (once
        // the list itself changed: an updated mod alone can't be undone here), Restart now
        bool can_merge=ahead==0&&!autom;
        if(can_merge){
            if(ImGui::Button(menu_text(UI_MODS_MERGE_NOW))){if(romfs_mods_premerge()){ahead=1;next=now+0.25;}}
            PRIO_LINE('M',menu_text(UI_MODS_MERGE_NOW));
#ifdef XCX_MOD_UI_TESTING
            prio_merge_low=ImGui::GetItemRectMin();prio_merge_high=ImGui::GetItemRectMax();
#endif
            ImGui::SameLine();
        }
        if(listchg){
            if(ImGui::Button(menu_text(UI_MODS_UNDO))){if(romfs_mods_restore_list())modlib_reread_enabled();next=0;}
            PRIO_LINE('U',menu_text(UI_MODS_UNDO));
#ifdef XCX_MOD_UI_TESTING
            prio_undo_low=ImGui::GetItemRectMin();prio_undo_high=ImGui::GetItemRectMax();
#endif
            if(ImGui::IsItemHovered())ImGui::SetTooltip("%s",menu_text(UI_MODS_UNDO_DESC));
            ImGui::SameLine();
        }
        ImGui::BeginDisabled(ahead==1||rt_restart_started());
        if(ImGui::Button(menu_text(UI_MODS_RESTART))){
            restart_failed=false;
            if(romfs_game_in_world())ImGui::OpenPopup("##restart");   // in the world: ask first
            else if(!rt_restart_game())restart_failed=true;
        }
        ImGui::EndDisabled();
        PRIO_LINE('Q',menu_text(UI_MODS_RESTART));
#ifdef XCX_MOD_UI_TESTING
        prio_restart_low=ImGui::GetItemRectMin();prio_restart_high=ImGui::GetItemRectMax();
#endif
        if(ImGui::BeginPopupModal("##restart",nullptr,ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoTitleBar)){
            ImGui::TextUnformatted(menu_text(UI_MODS_RESTART_CONFIRM));PRIO_LINE('C',menu_text(UI_MODS_RESTART_CONFIRM));
            ImGui::BeginDisabled(rt_restart_started());
            if(ImGui::Button(menu_text(UI_MODS_RESTART_YES))){ImGui::CloseCurrentPopup();if(!rt_restart_game())restart_failed=true;}
            ImGui::EndDisabled();
#ifdef XCX_MOD_UI_TESTING
            prio_yes_low=ImGui::GetItemRectMin();prio_yes_high=ImGui::GetItemRectMax();
#endif
            ImGui::SameLine();
            if(ImGui::Button(menu_text(UI_MODS_RESTART_NO))||ImGui::IsKeyPressed(ImGuiKey_Escape,false)||
               ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight,false))ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if(can_merge)ImGui::TextDisabled("%s",menu_text(UI_MODS_MERGE_NOW_DESC));
        if(restart_failed)ImGui::TextColored(ImVec4(1.0f,0.55f,0.5f,1),"%s",menu_text(UI_MODS_RESTART_FAILED));
        // then what the next start will have; with merge automatically on there's no button: romfs_mods.c merges a moment after the last change, so the
        // wait for that already counts as merging
        if(ahead==1||(ahead==0&&autom)){
            ImGui::TextDisabled("%s",menu_text(UI_MODS_MERGING));PRIO_LINE('W',menu_text(UI_MODS_MERGING));
        }else if(ahead==2){
            ImGui::TextDisabled("%s",menu_text(UI_MODS_MERGES_NEXT));PRIO_LINE('X',menu_text(UI_MODS_MERGES_NEXT));
            if(!merge_lines(true)){ImGui::TextUnformatted(menu_text(UI_MODS_MERGES_NEXT_NONE));PRIO_LINE('Z',menu_text(UI_MODS_MERGES_NEXT_NONE));}
        }
        // what the game is running with, out of the way under a heading of its own
        if(merges){
            ImGui::Spacing();
            PRIO_LINE('S',menu_text(UI_MODS_MERGES_START));
            if(ImGui::TreeNode("##start","%s",menu_text(UI_MODS_MERGES_START))){
                ImGui::TextDisabled("%s",menu_text(UI_MODS_MERGES));PRIO_LINE('H',menu_text(UI_MODS_MERGES));
                merge_lines(false);
                ImGui::TreePop();
            }
        }
        ImGui::PopTextWrapPos();
    }
}

// The mod library (runtime/mod_library.c): its mods and the installed ones on the left, the picked one's text and
// buttons on the right. Jobs run on the library's thread; this only reads its view and asks. Returns whether a row
// took the first focus.
static bool library(float scale,bool focus_first) {
    static ModLibView v;modlib_snapshot(&v);
    static char picked[65],confirm_remove[65];
    ImGui::SeparatorText(menu_text(UI_MODS_LIBRARY));
    bool idle=v.state==MODLIB_IDLE;
    if(v.state==MODLIB_LOADING)ImGui::TextDisabled("%s",menu_text(UI_MODS_LOADING));
    else if(v.state==MODLIB_WORKING&&v.total)ImGui::TextDisabled(menu_text(UI_MODS_DOWNLOADING),v.job,v.done/1048576.0,v.total/1048576.0);
    else if(v.state==MODLIB_WORKING)ImGui::TextDisabled(menu_text(UI_MODS_WORKING),v.job);
    if(v.message[0]){
        if(v.message_error)ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.0f,0.55f,0.5f,1));
        ImGui::TextWrapped("%s",v.message);
        if(v.message_error)ImGui::PopStyleColor();
    }
    for(int i=0;i<v.nnotes;++i){   // folders put in mods/ by hand: moved to the layout the game reads, or skipped and why
        if(v.notes[i].error)ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1.0f,0.55f,0.5f,1));
        ImGui::TextWrapped("%s",v.notes[i].text);
        if(v.notes[i].error)ImGui::PopStyleColor();
    }
    if(!v.count){
        ImGui::TextWrapped("%s",menu_text(UI_MODS_EMPTY));
        ImGui::BeginDisabled(!idle);if(ImGui::Button(menu_text(UI_MODS_REFRESH)))modlib_refresh();ImGui::EndDisabled();
        return false;
    }
    int index=0;for(int i=0;i<v.count;++i)if(!strcmp(picked,v.rows[i].id))index=i;
    snprintf(picked,sizeof picked,"%s",v.rows[index].id);
    bool focused=false;
    if(ImGui::BeginTable("##library",2,ImGuiTableFlags_SizingStretchProp)){
        ImGui::TableSetupColumn("##library_list",ImGuiTableColumnFlags_WidthFixed,300*scale);
        ImGui::TableSetupColumn("##library_detail",ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextColumn();
        for(int i=0;i<v.count;++i){
            const ModLibRow &r=v.rows[i];ImGui::PushID(r.id);
            if(focus_first&&i==index){ImGui::SetKeyboardFocusHere();focused=true;}
            if(ImGui::Selectable(r.name[0]?r.name:r.id,i==index)){snprintf(picked,sizeof picked,"%s",r.id);index=i;}
            UiTextId state=!r.installed?UI_MODS_STATE_AVAILABLE:r.update&&r.in_library?UI_MODS_STATE_UPDATE:r.enabled?UI_MODS_STATE_ON:UI_MODS_STATE_OFF;
            ImGui::TextDisabled("%s",menu_text(state));ImGui::Spacing();ImGui::PopID();
        }
        ImGui::BeginDisabled(!idle);if(ImGui::Button(menu_text(UI_MODS_REFRESH)))modlib_refresh();ImGui::EndDisabled();
        ImGui::TableNextColumn();
        const ModLibRow &r=v.rows[index];ImGui::PushID(r.id);
        ImGui::TextUnformatted(r.name[0]?r.name:r.id);
        if(r.author[0])ImGui::TextDisabled(menu_text(UI_MODS_BY),r.author);
        if(r.description[0])ImGui::TextWrapped("%s",r.description);
        if(r.notes[0]){ImGui::PushTextWrapPos(0);ImGui::TextDisabled("%s",r.notes);ImGui::PopTextWrapPos();}
        ImGui::PushTextWrapPos(0);ImGui::TextDisabled("%s",menu_text(strcmp(r.kind,"lua")?UI_MODS_KIND_ROMFS:UI_MODS_KIND_LUA));ImGui::PopTextWrapPos();
        if(r.installed&&r.enabled&&!strcmp(r.kind,"romfs")){   // its place in the Mod priority list below
            static char rank_id[65];static int rank,ranks;static double rank_next;static uint64_t rank_gen=~0ull;
            if(strcmp(rank_id,r.id)||(v.generation!=rank_gen&&idle)||ImGui::GetTime()>=rank_next){   // as mod_priority
                rank_gen=v.generation;
                char ids[32][65];ranks=romfs_mods_priority(ids,32);rank=0;
                for(int k=0;k<ranks;++k)if(!strcmp(ids[k],r.id))rank=k+1;
                snprintf(rank_id,sizeof rank_id,"%s",r.id);rank_next=ImGui::GetTime()+1.0;
            }
            if(rank){
                ImGui::TextDisabled(menu_text(UI_MODS_PRIORITY_OF),rank,ranks);
#ifdef XCX_MOD_UI_TESTING
                char t[96];snprintf(t,sizeof t,menu_text(UI_MODS_PRIORITY_OF),rank,ranks);PRIO_LINE('K',t);
#endif
            }
        }
        if(r.in_library&&!strcmp(r.kind,"romfs")&&(!r.installed||r.update)&&r.homepage[0]){
            ImGui::PushTextWrapPos(0);ImGui::TextDisabled(menu_text(UI_MODS_SOURCE),r.size/1048576.0,r.homepage);ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(!idle);
        if(r.in_library&&(!r.installed||r.update)){
            if(ImGui::Button(menu_text(r.installed?UI_MODS_UPDATE:UI_MODS_INSTALL))){modlib_install(r.id);confirm_remove[0]=0;}
            ImGui::SameLine();
        }
        if(r.installed){
            bool on=r.enabled!=0;
            if(ImGui::Checkbox(menu_text(UI_MODS_ON),&on))modlib_set_enabled(r.id,on);
            ImGui::SameLine(0,24*scale);
            bool sure=!strcmp(confirm_remove,r.id);
            if(ImGui::Button(menu_text(sure?UI_MODS_REMOVE_CONFIRM:UI_MODS_REMOVE))){
                if(sure){modlib_remove(r.id);confirm_remove[0]=0;}else snprintf(confirm_remove,sizeof confirm_remove,"%s",r.id);
            }
        }
        ImGui::EndDisabled();
        if(strcmp(confirm_remove,r.id))confirm_remove[0]=0;   // another mod picked: the second press starts over
        ImGui::PopID();ImGui::EndTable();
    }
    mod_priority(v,idle);
    return focused;
}

void mods_page(float scale,float bottom,bool focus_first) {
#ifdef XCX_MOD_UI_TESTING
    mods_widget_count=0;mods_retry_visible=false;mods_button_count=0;prio_count=0;memset(prio_off_shown,0,sizeof prio_off_shown);
#endif
    mod_ui_interaction(1);
    static ModSnapshot snapshot;mod_ui_snapshot(&snapshot);
    static char selected[65];
    if(requested_owner[0]){for(int i=0;i<snapshot.count;++i)if(!strcmp(snapshot.mods[i].id,requested_owner)&&!strcmp(snapshot.mods[i].page_id,requested_page))snprintf(selected,sizeof selected,"%s",requested_owner);requested_owner[0]=0;}
    if(ImGui::BeginChild("##mods_page",ImVec2(0,ImGui::GetContentRegionAvail().y-bottom),ImGuiChildFlags_NavFlattened)) {
        if(library(scale,focus_first))focus_first=false;
        ImGui::Spacing();ImGui::SeparatorText(menu_text(UI_MODS_LUA));
        if(!snapshot.count)ImGui::TextWrapped("%s",menu_text(UI_MODS_NO_LUA));
    }
    int index=0;for(int i=0;i<snapshot.count;++i)if(!strcmp(selected,snapshot.mods[i].id))index=i;
    if(snapshot.count)snprintf(selected,sizeof selected,"%s",snapshot.mods[index].id);
    {
        if(snapshot.count&&ImGui::BeginTable("##mods_columns",2,ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("##mod_list",ImGuiTableColumnFlags_WidthFixed,260*scale);
            ImGui::TableSetupColumn("##mod_settings",ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextColumn();
            for(int i=0;i<snapshot.count;++i) {
                const ModView &mod=snapshot.mods[i];ImGui::PushID(mod.id);
                if(focus_first && i==index)ImGui::SetKeyboardFocusHere();
                if(ImGui::Selectable(mod.name[0]?mod.name:mod.id,i==index)) {
                    if(index!=i){mod_ui_interaction(0);mod_ui_interaction(1);mod_ui_snapshot(&snapshot);}
                    index=i;snprintf(selected,sizeof selected,"%s",mod.id);
                }
#ifdef XCX_MOD_UI_TESTING
                mods_row_low[i]=ImGui::GetItemRectMin();mods_row_high[i]=ImGui::GetItemRectMax();
#endif
                ImGui::TextDisabled("%s",status_name(mod.status));ImGui::Spacing();ImGui::PopID();
            }
            ImGui::TableNextColumn();const ModView &mod=snapshot.mods[index];ImGui::PushID(mod.id);
            ImGui::TextUnformatted(mod.name[0]?mod.name:mod.id);ImGui::TextDisabled("%s  %s",mod.id,mod.version);
            ImGui::Text("Status: %s",status_name(mod.status));
            if(mod.diagnostic[0])ImGui::TextWrapped("%s",mod.diagnostic);
            if(mod.title[0]) {ImGui::Separator();ImGui::TextUnformatted(mod.title);}
            ImGui::BeginDisabled(mod.status!=MOD_LOADED);
            for(int i=0;i<mod.field_count;++i) {
                const ModField &f=mod.fields[i];ImGui::PushID(f.id);ImGui::TextUnformatted(f.label);
                bool changed=false;int value=f.value;
                ImGui::SetNextItemWidth(-1);
                if(f.type==MOD_BOOL) {bool checked=value!=0;changed=ImGui::Checkbox("##value",&checked);value=checked;}
                else if(f.type==MOD_INTEGER)changed=ImGui::SliderInt("##value",&value,f.minimum,f.maximum,"%d",ImGuiSliderFlags_AlwaysClamp|ImGuiSliderFlags_NoInput);
                else if(f.type==MOD_ENUM && ImGui::BeginCombo("##value",f.options[value].label)) {
                    for(int j=0;j<f.option_count;++j) {ImGui::PushID(f.options[j].id);if(ImGui::Selectable(f.options[j].label,j==value)){value=j;changed=true;}if(j==value)ImGui::SetItemDefaultFocus();ImGui::PopID();}
                    ImGui::EndCombo();
                }
#ifdef XCX_MOD_UI_TESTING
                if(mods_widget_count<128) {ModsWidget &w=mods_widgets[mods_widget_count++];w.id=GImGui->LastItemData.ID;w.window=ImGui::GetCurrentWindow();w.low=ImGui::GetItemRectMin();w.high=ImGui::GetItemRectMax();snprintf(w.mod,sizeof w.mod,"%s",mod.id);snprintf(w.field,sizeof w.field,"%s",f.id);}
#endif
                if(changed) {
                    ModAction a={};a.generation=snapshot.generation;a.session=snapshot.session;a.type=f.type;a.value=value;
                    snprintf(a.mod,sizeof a.mod,"%s",mod.id);snprintf(a.field,sizeof a.field,"%s",f.id);rt_mods_ui_submit(&a);
                }
                ImGui::PopID();
            }
            ImGui::EndDisabled();
            if(mod.save_note[0])ImGui::TextWrapped("%s",mod.save_note);
            if(mod.persistence_dirty && mod.field_count) {
                ImGui::BeginDisabled(mod.status!=MOD_LOADED);
                if(ImGui::Button("Retry save")) {
                    // A same-value action retries the complete page without repeating Lua events.
                    const ModField &f=mod.fields[0];ModAction a={};a.generation=snapshot.generation;a.session=snapshot.session;a.type=f.type;a.value=f.value;
                    snprintf(a.mod,sizeof a.mod,"%s",mod.id);snprintf(a.field,sizeof a.field,"%s",f.id);rt_mods_ui_submit(&a);
                }
#ifdef XCX_MOD_UI_TESTING
                mods_retry_visible=true;mods_retry_low=ImGui::GetItemRectMin();mods_retry_high=ImGui::GetItemRectMax();
#endif
                ImGui::EndDisabled();
            }
            if(mod.button_count){
                ImGui::Separator();ImGui::PushID("actions");bool provider_buttons=false;
                for(int i=0;i<mod.button_count;++i){const ModButton &button=mod.buttons[i];provider_buttons|=button.provider!=0;ImGui::PushID(button.id);
                    ImGui::BeginDisabled(mod.status!=MOD_LOADED||!button.enabled||(button.provider&&(!mod.provider_available||mod.provider_pending)));
                    if(ImGui::Button(button.label)){ModIntent a={};a.generation=snapshot.generation;a.session=snapshot.session;snprintf(a.mod,sizeof a.mod,"%s",mod.id);snprintf(a.action,sizeof a.action,"%s",button.id);rt_mods_ui_intent(&a);}
#ifdef XCX_MOD_UI_TESTING
                    if(mods_button_count<4){mods_button_id[mods_button_count]=GImGui->LastItemData.ID;mods_button_window[mods_button_count]=ImGui::GetCurrentWindow();mods_button_low[mods_button_count]=ImGui::GetItemRectMin();mods_button_high[mods_button_count++]=ImGui::GetItemRectMax();}
#endif
                    ImGui::EndDisabled();ImGui::PopID();
                }
                ImGui::PopID();
                if(mod.provider_pending)ImGui::Text("Pending work: %d",mod.provider_pending);
                if(provider_buttons&&mod.provider_note[0])ImGui::TextWrapped("%s",mod.provider_note);
            }
            if(mod.ui_status[0])ImGui::TextWrapped("%s",mod.ui_status);
            if(snapshot.pending)ImGui::TextDisabled("Applying settings...");
            if(snapshot.notice[0])ImGui::TextWrapped("%s",snapshot.notice);
            ImGui::Separator();ImGui::TextWrapped("Package and enable/disable changes take effect after restart.");
            ImGui::PopID();ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}
