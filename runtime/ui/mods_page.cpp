#include "mods_page.h"
#include "imgui.h"
#include <cfloat>
#include <cstdio>
#include <cstring>
extern "C" {
#include "mods.h"
#include "mod_settings.h"
#include "mod_library.h"
#include "ui_text.h"
}
#include "imgui_internal.h"   // SetFocusID, NavMoveRequestCancel
#ifdef XCX_MOD_UI_TESTING
struct ModsWidget {ImGuiID id;ImGuiWindow *window;ImVec2 low,high;char mod[65],field[65];};
static ModsWidget mods_widgets[128];static int mods_widget_count;
static ImVec2 mods_row_low[MOD_UI_MAX],mods_row_high[MOD_UI_MAX];
static ImGuiID mods_button_id[4];static ImGuiWindow *mods_button_window[4];
static ImVec2 mods_button_low[4],mods_button_high[4];static int mods_button_count;
static ImVec2 mods_retry_low,mods_retry_high;static bool mods_retry_visible;
// the library list as drawn (tests/mods_library_check.cpp): its rows' labels, its entries with parts (label, parts on
// text, rect), the height of its scroll box and the most the box may take
static char lib_rows[128][100];static int lib_row_count;
static char lib_groups[16][100],lib_group_state[16][64];static ImVec2 lib_group_low[16],lib_group_high[16];static int lib_group_count;
static float lib_box_height,lib_box_max;
static ImGuiID lib_on_id;   // the picked mod's On box
static char lib_pane_mod[65];static int lib_wrapped;   // the mod the pane shows; rows whose title takes two lines
static float lib_focus_line_bottom,lib_box_bottom;   // the focused row's line under its name; the list box's view
#endif
static const char *status_name(int status) {
    switch(status) {case MOD_LOADED:return "Loaded";case MOD_DISABLED:return "Disabled";case MOD_INCOMPATIBLE:return "Incompatible";case MOD_FAULTED:return "Faulted";default:return "Starting";}
}
// The name of the library entry a part belongs to: library.ini's group_name (installer/mods.py, write_info, always
// writes it with a group), else, for a library.ini written by hand without one, what the part's own name says before
// ": " (part_name: "Tweaks mod: Probe tweak"), else the entry's id.
static void group_label(const ModLibRow &r,char *out,size_t cap) {
    const char *colon=strstr(r.name,": ");
    if(r.group_name[0])snprintf(out,cap,"%s",r.group_name);
    else if(colon)snprintf(out,cap,"%.*s",(int)(colon-r.name),r.name);
    else snprintf(out,cap,"%s",r.group);
}
// A part's row under its entry: its name without the entry's name in front.
static const char *part_label(const ModLibRow &r,const char *group) {
    size_t n=strlen(group);
    if(n&&!strncmp(r.name,group,n)&&!strncmp(r.name+n,": ",2)&&r.name[n+2])return r.name+n+2;
    return r.name[0]?r.name:r.id;
}
// Between the library's list and the picked mod's pane with a controller or the arrow keys. Directional navigation only
// moves Right or Left to an item more beside than below or above, and the pane's first button sits under the mod's
// name and text, often well below its row; with a controller it also drops the focus first when the focused item's
// box reaches out of its window's visible area, which a list row's does (rows span the list box's padding). So the row
// or pane button focused at the end of a frame is remembered, and at the next frame's start (nav_switch, before
// anything is drawn) Right from a row picks it and goes to the pane's first button, Left from the pane goes back to
// the picked row, and ImGui's own move is cancelled. Up and Down inside the list are ours too: the list's rows (mod
// rows and entry rows) are recorded in the order drawn, and a move goes to the row before or after the focused one,
// which ImGui's own moves in this nested box didn't always find. From the first or last row, ImGui's move goes on
// (up to the tabs, down to Refresh).
static bool detail_focus,list_focus;
static char row_focused[65];static int row_focused_frame=-9,pane_focused_frame=-9;
static ImGuiID list_rows[256],list_rows_last[256],list_focused,want_focus;   // this frame's rows, the last frame's
static int nlist,nlist_last,list_focused_frame=-9;
static bool nav_pressed(bool right) {
    return right?ImGui::IsKeyPressed(ImGuiKey_RightArrow,false)||ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight,false)||
                 ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight,false)
               :ImGui::IsKeyPressed(ImGuiKey_LeftArrow,false)||ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft,false)||
                 ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft,false);
}
static bool nav_vertical(bool down,bool repeat=true) {   // held, it repeats as ImGui's own moves do (repeat)
    return down?ImGui::IsKeyPressed(ImGuiKey_DownArrow,repeat)||ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown,repeat)||
                ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown,repeat)
              :ImGui::IsKeyPressed(ImGuiKey_UpArrow,repeat)||ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp,repeat)||
                ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp,repeat);
}
// At the start of the library, before its rows: Right, Left, Up or Down from what was focused at the end of the last
// frame.
static void nav_switch(char *picked,size_t cap) {
    int last=ImGui::GetFrameCount()-1;
    memcpy(list_rows_last,list_rows,sizeof list_rows);nlist_last=nlist;nlist=0;
    bool down=nav_vertical(true),up=!down&&nav_vertical(false);
    if(row_focused_frame==last&&nav_pressed(true)){
        ImGui::NavMoveRequestCancel();snprintf(picked,cap,"%s",row_focused);detail_focus=true;
    }else if(pane_focused_frame==last&&nav_pressed(false)){
        ImGui::NavMoveRequestCancel();list_focus=true;
    }else if(list_focused_frame==last&&(down||up)){
        int k=0;while(k<nlist_last&&list_rows_last[k]!=list_focused)k++;
        int to=k+(down?1:-1);
        if(k<nlist_last&&to>=0&&to<nlist_last){ImGui::NavMoveRequestCancel();want_focus=list_rows_last[to];}
    }
}
static void focus_here() {   // the last item drawn, set straight away and highlighted, so A (which presses only a
    ImGui::SetFocusID(ImGui::GetItemID(),ImGui::GetCurrentWindow());   // highlighted item) works on it
    ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);ImGui::SetNavCursorVisible(true);
}
// After each of the pane's buttons: the first one drawn takes the focus Right asked for; a focused one is remembered.
static void pane_item() {
    if(detail_focus){focus_here();detail_focus=false;}
    if(ImGui::IsItemFocused())pane_focused_frame=ImGui::GetFrameCount();
}
static void focus_last() { focus_here();list_focus=false; }   // back in the list, on the picked row
// After each of the list's rows (a mod's, or an entry's): kept in the drawn order, focused when Up or Down asked for
// it, and remembered when focused. 1 when it took the focus here.
static bool list_item() {
    ImGuiID id=ImGui::GetItemID();bool took=false;
    if(nlist<256)list_rows[nlist++]=id;
    if(want_focus==id){focus_here();want_focus=0;took=true;}
    if(ImGui::IsItemFocused()){list_focused=id;list_focused_frame=ImGui::GetFrameCount();}
    return took;
}

// A row is its name and the line under it (On, Off, "2 of 6 parts on"). When the focus has just moved onto the name
// (the d-pad, the arrow keys, Left back from the pane), the list scrolls so that line shows too: ImGui's own scroll
// keeps only the name in view. Called after the line, with the name's id and top left.
static void keep_row_in_view(ImGuiID id,ImVec2 top,bool moved_here) {
#ifdef XCX_MOD_UI_TESTING
    if(GImGui->NavId==id)lib_focus_line_bottom=ImGui::GetItemRectMax().y;
#endif
    if(!moved_here&&GImGui->NavJustMovedToId!=id)return;
    ImRect row(top,ImGui::GetItemRectMax());row.Max.y+=ImGui::GetStyle().ItemSpacing.y;
    ImGui::ScrollToRect(ImGui::GetCurrentWindow(),row,ImGuiScrollFlags_KeepVisibleEdgeY);
}

// A row's title: one line, or for a long one two lines wrapped to the list's width (the rest cut), so it can be read.
static bool row_title(const char *label,bool selected) {
    float wrap=ImGui::GetContentRegionAvail().x,line=ImGui::GetTextLineHeight();
    if(ImGui::CalcTextSize(label,nullptr,false,wrap).y<line*1.5f)return ImGui::Selectable(label,selected);
#ifdef XCX_MOD_UI_TESTING
    lib_wrapped++;
#endif
    ImVec2 at=ImGui::GetCursorScreenPos();
    bool pressed=ImGui::Selectable("##title",selected,0,ImVec2(0,line*2));
    ImVec4 clip(at.x,at.y,at.x+wrap,at.y+line*2);
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),ImGui::GetFontSize(),at,ImGui::GetColorU32(ImGuiCol_Text),label,
                                        nullptr,wrap,&clip);
    return pressed;
}

static void library_row(const ModLibRow &r,int i,int &index,char *picked,size_t cap,bool focus,const char *label) {
    ImGui::PushID(r.id);
    if(row_title(label,i==index)){snprintf(picked,cap,"%s",r.id);index=i;}
    ImGuiID id=ImGui::GetItemID();ImVec2 top=ImGui::GetItemRectMin();
    bool back=(list_focus&&i==index)||focus;
    if(back)focus_last();   // back from the pane, or the tab's first focus (as the other tabs' first row)
    back|=list_item();
    if(ImGui::IsItemFocused()){   // the row the cursor is on is the picked one: the pane shows it without pressing A
        snprintf(row_focused,sizeof row_focused,"%s",r.id);row_focused_frame=ImGui::GetFrameCount();
        if(index!=i){snprintf(picked,cap,"%s",r.id);index=i;}
    }
#ifdef XCX_MOD_UI_TESTING
    if(lib_row_count<128)snprintf(lib_rows[lib_row_count++],sizeof lib_rows[0],"%s",label);
#endif
    UiTextId state=!r.installed?UI_MODS_STATE_AVAILABLE:r.update&&r.in_library?UI_MODS_STATE_UPDATE:r.enabled?UI_MODS_STATE_ON:UI_MODS_STATE_OFF;
    ImGui::TextDisabled("%s",menu_text(state));keep_row_in_view(id,top,back);ImGui::Spacing();ImGui::PopID();
}

static char requested_owner[65],requested_page[65];
void mods_page_select(const char *owner,const char *page){snprintf(requested_owner,sizeof requested_owner,"%s",owner);snprintf(requested_page,sizeof requested_page,"%s",page);}

// The mod library (runtime/mod_library.c): its mods and the installed ones on the left, the picked one's text and
// buttons on the right. Jobs run on the library's thread; this only reads its view and asks. Returns whether a row
// took the first focus.
static bool lib_focusable;   // the library drew something the tab's first focus can take: a row, or Refresh enabled
static bool library_focusable() { return lib_focusable; }
static bool library(float scale,bool focus_first) {
    static ModLibView v;modlib_snapshot(&v);
    lib_focusable=v.count>0||v.state==MODLIB_IDLE;
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
    if(!v.count){   // nothing listed (offline, or still loading): Refresh takes the tab's first focus while it can
        ImGui::TextWrapped("%s",menu_text(UI_MODS_EMPTY));
        ImGui::BeginDisabled(!idle);if(ImGui::Button(menu_text(UI_MODS_REFRESH)))modlib_refresh();ImGui::EndDisabled();
        if(focus_first&&idle){focus_here();return true;}
        return false;
    }
    nav_switch(picked,sizeof picked);   // Right / Left between the list and the pane, before anything is drawn
    int index=0;for(int i=0;i<v.count;++i)if(!strcmp(picked,v.rows[i].id))index=i;
    snprintf(picked,sizeof picked,"%s",v.rows[index].id);
    bool focused=false;
    if(ImGui::BeginTable("##library",2,ImGuiTableFlags_SizingStretchProp)){
        ImGui::TableSetupColumn("##library_list",ImGuiTableColumnFlags_WidthFixed,360*scale);
        ImGui::TableSetupColumn("##library_detail",ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextColumn();
        // The rows scroll in a box of their own, at most about two thirds of the page high, so the picked mod's text, its On box
        // and its buttons in the next column stay in view beside a long list.
        float rows_max=ImGui::GetWindowHeight()*0.7f;if(rows_max<160*scale)rows_max=160*scale;
        ImGui::SetNextWindowSizeConstraints(ImVec2(0,0),ImVec2(FLT_MAX,rows_max));
        ImGui::BeginChild("##library_rows",ImVec2(0,0),ImGuiChildFlags_AutoResizeY|ImGuiChildFlags_NavFlattened);
#ifdef XCX_MOD_UI_TESTING
        lib_box_bottom=ImGui::GetCurrentWindow()->InnerRect.Max.y;lib_focus_line_bottom=0;
#endif
        // An entry's parts (library.ini group) are one row that opens on them, shut at first, so an entry with many
        // optional pieces takes one row; it opens by itself when the picked mod is one of them.
        for(int i=0;i<v.count;++i){
            const ModLibRow &r=v.rows[i];
            if(!r.group[0]){
                bool focus=focus_first&&i==index;focused|=focus;
                library_row(r,i,index,picked,sizeof picked,focus,r.name[0]?r.name:r.id);continue;
            }
            bool first=true;for(int k=0;k<i&&first;++k)if(!strcmp(v.rows[k].group,r.group))first=false;
            if(!first)continue;   // drawn under its entry's row
            char group[96];group_label(r,group,sizeof group);
            int parts=0,on=0;bool holds_pick=false;
            for(int k=i;k<v.count;++k)if(!strcmp(v.rows[k].group,r.group)){parts++;on+=v.rows[k].enabled!=0;holds_pick|=k==index;}
            ImGui::PushID("group");ImGui::PushID(r.group);
            if(holds_pick)ImGui::SetNextItemOpen(true,ImGuiCond_Appearing);
            bool focus=focus_first&&holds_pick;focused|=focus;
            bool open=ImGui::TreeNodeEx("##parts",ImGuiTreeNodeFlags_SpanAvailWidth|ImGuiTreeNodeFlags_NoTreePushOnOpen,"%s",group);
            ImGuiID node=ImGui::GetItemID();ImVec2 node_top=ImGui::GetItemRectMin();
            bool node_back=(list_focus&&holds_pick&&!open)||focus;
            if(node_back)focus_last();   // the picked part is shut away (or the tab's first focus): its entry's row
            node_back|=list_item();
#ifdef XCX_MOD_UI_TESTING
            if(lib_group_count<16){
                int g=lib_group_count++;snprintf(lib_groups[g],sizeof lib_groups[0],"%s",group);
                snprintf(lib_group_state[g],sizeof lib_group_state[0],menu_text(UI_MODS_STATE_PARTS),on,parts);
                lib_group_low[g]=ImGui::GetItemRectMin();lib_group_high[g]=ImGui::GetItemRectMax();
            }
#endif
            ImGui::TextDisabled(menu_text(UI_MODS_STATE_PARTS),on,parts);
            keep_row_in_view(node,node_top,node_back);ImGui::Spacing();
            ImGui::PopID();ImGui::PopID();
            if(open){
                ImGui::Indent();
                for(int k=i;k<v.count;++k)if(!strcmp(v.rows[k].group,r.group))
                    library_row(v.rows[k],k,index,picked,sizeof picked,false,part_label(v.rows[k],group));
                ImGui::Unindent();
            }
        }
        ImGui::EndChild();
        list_focus=false;want_focus=0;   // asked for and not drawn (the list changed): dropped
        if(v.hidden)ImGui::TextDisabled(menu_text(UI_MODS_HIDDEN),v.hidden,MODLIB_ROWS);
#ifdef XCX_MOD_UI_TESTING
        lib_box_height=ImGui::GetItemRectSize().y;lib_box_max=rows_max;
#endif
        ImGui::BeginDisabled(!idle);if(ImGui::Button(menu_text(UI_MODS_REFRESH)))modlib_refresh();ImGui::EndDisabled();
        ImGui::TableNextColumn();
        const ModLibRow &r=v.rows[index];ImGui::PushID(r.id);
#ifdef XCX_MOD_UI_TESTING
        snprintf(lib_pane_mod,sizeof lib_pane_mod,"%s",r.id);
#endif
        ImGui::TextUnformatted(r.name[0]?r.name:r.id);
        if(r.author[0])ImGui::TextDisabled(menu_text(UI_MODS_BY),r.author);
        if(r.description[0])ImGui::TextWrapped("%s",r.description);
        if(r.notes[0]){ImGui::PushTextWrapPos(0);ImGui::TextDisabled("%s",r.notes);ImGui::PopTextWrapPos();}
        ImGui::PushTextWrapPos(0);ImGui::TextDisabled("%s",menu_text(strcmp(r.kind,"lua")?UI_MODS_KIND_ROMFS:UI_MODS_KIND_LUA));ImGui::PopTextWrapPos();
        if(r.in_library&&!strcmp(r.kind,"romfs")&&(!r.installed||r.update)&&r.homepage[0]){
            ImGui::PushTextWrapPos(0);ImGui::TextDisabled(menu_text(UI_MODS_SOURCE),r.size/1048576.0,r.homepage);ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(!idle);
        if(r.in_library&&(!r.installed||r.update)){
            if(ImGui::Button(menu_text(r.installed?UI_MODS_UPDATE:UI_MODS_INSTALL))){modlib_install(r.id);confirm_remove[0]=0;}
            pane_item();
            ImGui::SameLine();
        }
        if(r.installed){
            bool on=r.enabled!=0;
            if(ImGui::Checkbox(menu_text(UI_MODS_ON),&on))modlib_set_enabled(r.id,on);
#ifdef XCX_MOD_UI_TESTING
            lib_on_id=ImGui::GetItemID();
#endif
            pane_item();
            ImGui::SameLine(0,24*scale);
            bool sure=!strcmp(confirm_remove,r.id);
            if(ImGui::Button(menu_text(sure?UI_MODS_REMOVE_CONFIRM:UI_MODS_REMOVE))){
                if(sure){modlib_remove(r.id);confirm_remove[0]=0;}else snprintf(confirm_remove,sizeof confirm_remove,"%s",r.id);
            }
            pane_item();
        }
        ImGui::EndDisabled();
        detail_focus=false;   // nothing to focus (busy, or no button): dropped
        if(strcmp(confirm_remove,r.id))confirm_remove[0]=0;   // another mod picked: the second press starts over
        ImGui::PopID();ImGui::EndTable();
    }
    return focused;
}

void mods_page(float scale,float bottom,bool focus_first) {
#ifdef XCX_MOD_UI_TESTING
    mods_widget_count=0;mods_retry_visible=false;mods_button_count=0;lib_row_count=0;lib_group_count=0;lib_wrapped=0;
#endif
    mod_ui_interaction(1);
    static ModSnapshot snapshot;mod_ui_snapshot(&snapshot);
    static char selected[65];
    if(requested_owner[0]){for(int i=0;i<snapshot.count;++i)if(!strcmp(snapshot.mods[i].id,requested_owner)&&!strcmp(snapshot.mods[i].page_id,requested_page))snprintf(selected,sizeof selected,"%s",requested_owner);requested_owner[0]=0;}
    // Down from the menu's tabs (above this page, in the menu's own window) goes to the library's picked row, as a tab
    // switch does. ImGui's own move would end on the menu's Continue button: the library rows sit in a box of their
    // own, a second child level, whose rows scrolled out of view ImGui doesn't clip from the search (ui/menu.cpp, the
    // page's NavFlattened child).
    // Only while something on the page can take the focus (last frame's count): else ImGui's own move goes on to the
    // menu's buttons below.
    static bool can_focus;
    {
        ImGuiContext &g=*GImGui;ImGuiWindow *nav=g.NavWindow;
        bool down=nav_vertical(true,false);
        if(down&&can_focus&&nav==ImGui::GetCurrentWindow()&&   // on the tabs, or on nothing (a page left behind took the focus away)
           (!g.NavId||ImGui::WindowRectRelToAbs(nav,nav->NavRectRel[g.NavLayer]).Max.y<=ImGui::GetCursorScreenPos().y)){
            ImGui::NavMoveRequestCancel();ImGui::SetNavCursorVisible(true);focus_first=true;
        }
    }
    // Arriving at the tab, the first row takes the focus as on the other tabs (ui/option_rows.cpp). On the tab's first
    // frame ImGui can still have the focus on the page left behind, or skip the library's box while it sizes itself
    // (a log of a switch with R1 showed the focus staying on the last tab's row, then nothing): so the request holds,
    // tried each frame, until the focus is in this page, for up to FIRST_FRAMES frames.
    enum { FIRST_FRAMES = 8 };
    static int first_frames;
    if(focus_first)first_frames=FIRST_FRAMES;
    focus_first=first_frames>0;
    if(focus_first)ImGui::NavMoveRequestCancel();   // this frame's move goes to the first row instead (option_rows.cpp)
    bool first_try=focus_first;
    if(ImGui::BeginChild("##mods_page",ImVec2(0,ImGui::GetContentRegionAvail().y-bottom),ImGuiChildFlags_NavFlattened)) {
        if(focus_first)ImGui::SetScrollY(0);   // the page's top, as the other tabs
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
                if(ImGui::Selectable(mod.name[0]?mod.name:mod.id,i==index)) {
                    if(index!=i){mod_ui_interaction(0);mod_ui_interaction(1);mod_ui_snapshot(&snapshot);}
                    index=i;snprintf(selected,sizeof selected,"%s",mod.id);
                }
                if(focus_first && i==index)focus_here();   // the tab's first focus, as the other tabs' first row
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
    can_focus=snapshot.count>0||library_focusable();
    if(first_try){   // held when the focus is now in this page, else tried again next frame
        ImGuiWindow *nav=GImGui->NavWindow;
        bool held=GImGui->NavId&&nav&&strstr(nav->Name,"##mods_page");
        first_frames=held?0:first_frames-1;
    }
}
