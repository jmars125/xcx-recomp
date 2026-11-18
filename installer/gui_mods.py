"""The Mods window of the installer (installer/gui.py, the Mods button): a list of the mod library joined with the
mods in the install folder, and buttons to install, update, turn on, turn off and remove them. installer/mods.py does
the work; downloads run on a worker thread and report back through a queue the window polls.

Adding an archive, or a library mod the player downloads from its page (mods.is_manual), reads it first
(mods.preview) and shows what will be installed where when the archive is not in the plain romfs layout
(mod_layout.needs_confirm); an archive with several versions of a mod asks which one.

An entry that comes in parts (a full version and optional pieces) is one row with a row per part under it. Install
on it takes every downloaded file the player picks, matches each to its part by SHA-256 and installs those; files
that are no part of it can be added as mods of their own. Turning on a part that contains another one that is on (or
the other way round) asks first."""
import queue
import threading
import tkinter as tk
import webbrowser
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from urllib.parse import urlparse

from . import mod_layout, mods, theme

POLL_MS = 100
PAD = 14


class ModsWindow:
    def __init__(self, parent, cat, dest, ui_font, index=None):
        self.cat, self.dest, self.index_source = cat, Path(dest), index
        self.entries = []
        self.notes = []          # normalise's lines for the list on its way
        self.busy = False
        self.results = queue.Queue()
        w = self.win = tk.Toplevel(parent)
        w.title(cat('mods.title'))
        w.transient(parent)
        w.minsize(640, 420)
        w.configure(background=theme.BG)
        theme.dark_title_bar(w)
        f = ttk.Frame(w, padding=PAD)
        f.pack(fill='both', expand=True)
        ttk.Label(f, text=cat('mods.intro', dest=str(self.dest)), wraplength=700, justify='left').pack(anchor='w')
        if index:   # --mods-index: entries no one has checked, so say so above the list the whole time
            ttk.Label(f, text=cat('mods.custom_index', source=index), wraplength=700, justify='left',
                      foreground=theme.WARN).pack(anchor='w', pady=(6, 0))

        bar = ttk.Frame(f)   # packed before the list so the list shrinks, never the buttons
        bar.pack(side='bottom', fill='x', pady=(10, 0))
        self.buttons = {}
        for key, side in (('install', 'left'), ('toggle', 'left'), ('remove', 'left'), ('close', 'right'),
                          ('add_zip', 'right'), ('refresh', 'right')):
            b = ttk.Button(bar, text=cat('mods.' + key), command=getattr(self, 'on_' + key))
            b.pack(side=side, padx=(0, 8) if side == 'left' else (8, 0))
            self.buttons[key] = b
        self.status = ttk.Label(f, wraplength=700, justify='left')
        self.status.pack(side='bottom', anchor='w', pady=(8, 0))

        detail = ttk.Frame(f)
        detail.pack(side='bottom', fill='x', pady=(8, 0))
        self.detail = ttk.Label(detail, wraplength=700, justify='left')
        self.detail.pack(anchor='w')
        self.page = ttk.Button(detail, text=cat('mods.page'), command=self.on_page)

        cols = ('name', 'kind', 'author', 'state')
        box = ttk.Frame(f)
        box.pack(fill='both', expand=True, pady=(10, 0))
        self.rows = {}   # tree item -> its entry, or ('part', entry, part row)
        self.tree = ttk.Treeview(box, columns=cols, show='tree headings', selectmode='browse', height=10)
        self.tree.column('#0', width=28, minwidth=28, stretch=False)   # the open/close mark of entries with parts
        for c, width in zip(cols, (260, 110, 140, 150)):
            self.tree.heading(c, text=cat('mods.col_' + c))
            self.tree.column(c, width=width, stretch=c == 'name')
        scroll = ttk.Scrollbar(box, orient='vertical', command=self.tree.yview)
        self.tree.configure(yscrollcommand=scroll.set)
        self.tree.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')
        self.tree.bind('<<TreeviewSelect>>', lambda _e: self.refresh_selection())
        self.opened = set()   # entries with parts the player opened
        self.tree.bind('<<TreeviewOpen>>', lambda _e: self.opened.add(self.tree.focus()))
        self.tree.bind('<<TreeviewClose>>', lambda _e: self.opened.discard(self.tree.focus()))
        w.protocol('WM_DELETE_WINDOW', self.on_close)
        self.refresh_selection()
        self.load()
        w.after(POLL_MS, self.poll)
        w.update_idletasks()   # over the Setup window, not in a corner of the screen
        x = parent.winfo_rootx() + (parent.winfo_width() - w.winfo_reqwidth()) // 2
        y = parent.winfo_rooty() + (parent.winfo_height() - w.winfo_reqheight()) // 2
        w.geometry(f'+{max(0, x)}+{max(0, y)}')

    # ---- worker --------------------------------------------------------------------------------------------------

    def run(self, message, work, then=None, failed=None):
        """Runs work() on a worker thread. Its result goes to then(result) on the window's thread when given, else
        to the list or the status line; a ModError goes to failed(error) when that returns True, else to the status
        line."""
        if self.busy:
            return
        self.busy = True
        self.set_status(message)
        self.refresh_selection()

        def body():
            try:
                value = work()
                self.results.put(('then', (then, value)) if then else ('ok', value))
            except mods.ModError as e:
                self.results.put(('failed', (failed, e)) if failed else ('error', e))
            except OSError as e:
                self.results.put(('error', mods.ModError('mods.failed', detail=str(e))))
        threading.Thread(target=body, daemon=True).start()

    def poll(self):
        try:
            while True:
                kind, value = self.results.get_nowait()
                self.busy = False
                if kind == 'progress':
                    self.busy = True
                    self.set_status(self.cat('mods.downloading', done=value[0] >> 20, total=max(1, value[1] >> 20)))
                    continue
                if kind == 'notes':   # normalise's lines; shown once the list they belong to arrives
                    self.busy = True
                    self.notes = value
                    continue
                if kind == 'then':
                    self.set_status('')
                    self.refresh_selection()
                    value[0](value[1])
                    continue
                if kind == 'failed':
                    if not value[0](value[1]):
                        self.set_status(self.cat(value[1].id, **value[1].params), error=True)
                    self.refresh_selection()
                    continue
                if kind == 'error':
                    self.set_status(self.cat(value.id, **value.params), error=True)
                else:
                    if isinstance(value, list):
                        self.entries = value
                        notes, self.notes = self.notes, []
                        if notes:
                            self.set_status('\n'.join(self.cat(i, **p) for i, p in notes),
                                            error=any(i != 'mods.moved' for i, _ in notes))
                        elif self.status.cget('foreground') != theme.ERROR:
                            self.set_status('')   # the "reading" or "installing" line is done
                    elif isinstance(value, str):
                        self.set_status(value)
                    self.fill()
                self.refresh_selection()
        except queue.Empty:
            pass
        if self.win.winfo_exists():
            self.win.after(POLL_MS, self.poll)

    def progress(self, done, total):
        self.results.put(('progress', (done, total)))

    def load(self):
        def work():
            try:
                index = mods.fetch_index(self.index_source)
            except mods.ModError:
                index = []   # offline: still list and switch the installed mods
                self.results.put(('error', mods.ModError('mods.offline')))
            return self.listed(index)
        self.run(self.cat('mods.loading'), work)

    # ---- list ----------------------------------------------------------------------------------------------------

    def state_text(self, row):
        if not row['installed']:
            return self.cat('mods.state_available')
        if row.get('update'):
            return self.cat('mods.state_update')
        return self.cat('mods.state_on' if row['enabled'] else 'mods.state_off')

    def fill(self):
        keep = self.selected_id()
        self.tree.delete(*self.tree.get_children())
        self.rows = {}
        for e in self.entries:
            parts = e.get('parts') or []
            state = self.state_text(e)
            if parts:
                n_on = sum(1 for p in parts if p['enabled'])
                n_in = sum(1 for p in parts if p['installed'])
                state = self.cat('mods.state_parts', on=n_on, installed=n_in, total=len(parts)) if n_in else state
            # an entry with parts starts shut, so one with many optional pieces takes a row; the player's opening or
            # shutting it lasts while the window is open
            self.tree.insert('', 'end', iid=e['id'], open=e['id'] in self.opened, values=(
                e.get('name') or e['id'], self.cat('mods.kind_' + e['kind']), e.get('author', ''), state))
            self.rows[e['id']] = e
            for part in parts:
                self.tree.insert(e['id'], 'end', iid=part['mid'],
                                 values=('    ' + part['name'], '', '', self.state_text(part)))   # indented under the entry
                self.rows[part['mid']] = ('part', e, part)
        if keep and self.tree.exists(keep):
            parent = self.tree.parent(keep)
            if parent:   # a picked part keeps its entry open
                self.tree.item(parent, open=True)
                self.opened.add(parent)
            self.tree.selection_set(keep)

    def selected_id(self):
        sel = self.tree.selection()
        return sel[0] if sel else None

    def selected(self):
        """(entry, part): the selected row's entry, and its part row when a part is selected (else None)."""
        row = self.rows.get(self.selected_id())
        if isinstance(row, tuple):
            return row[1], row[2]
        return row, None

    def refresh_selection(self):
        e, part = self.selected()
        b = self.buttons
        state = lambda on: ['!disabled'] if on and not self.busy else ['disabled']
        in_library = bool(e and (e.get('url') or e.get('files') or e.get('parts') or mods.is_manual(e)))
        target = part or e   # the row Turn on / Remove act on
        many = bool(e and e.get('parts')) and part is None
        if many:   # an entry with parts: Install adds parts, switching and removing are per part
            b['install'].configure(text=self.cat('mods.install_parts'))
            b['install'].state(state(in_library))
            b['toggle'].configure(text=self.cat('mods.turn_on'))
            b['toggle'].state(state(False))
            b['remove'].state(state(False))
        else:
            b['install'].configure(text=self.cat('mods.update' if target and target['installed'] else 'mods.install'))
            b['install'].state(state(target and in_library and (not target['installed'] or target.get('update'))))
            b['toggle'].configure(text=self.cat('mods.turn_off' if target and target['enabled'] else 'mods.turn_on'))
            b['toggle'].state(state(target and target['installed']))
            b['remove'].state(state(target and target['installed']))
        b['add_zip'].state(state(True))
        b['refresh'].state(state(True))
        if not e:
            self.detail.configure(text=self.cat('mods.pick'))
            self.page.pack_forget()
            return
        text = e.get('description') or ''
        if part:
            text = part['name'] + (': ' + part['description'] if part.get('description') else '')
            inside = [p['name'] for p in e['parts'] if p['id'] in part.get('includes', ())]
            if inside:
                text += '\n' + self.cat('mods.part_includes', parts=', '.join(inside))
        if e.get('author'):
            text += ('\n' if text else '') + self.cat('mods.by', author=e['author'])
        if many:
            text += '\n' + self.cat('mods.has_parts', count=len(e['parts']))
        if e.get('notes'):
            text += '\n' + e['notes']
        if e['kind'] == 'lua':
            text += '\n' + self.cat('mods.lua_restart')
        self.detail.configure(text=text)
        if e.get('homepage'):
            self.page.pack(anchor='w', pady=(6, 0))
        else:
            self.page.pack_forget()

    def set_status(self, text, error=False):
        self.status.configure(text=text, foreground=theme.ERROR if error else '')

    # ---- buttons -------------------------------------------------------------------------------------------------

    @staticmethod
    def bare(e):
        """A library entry as the index lists it, without the window's state."""
        out = {k: v for k, v in e.items() if k not in ('installed', 'enabled', 'update')}
        if out.get('parts'):
            out['parts'] = [{k: v for k, v in p.items() if k not in ('installed', 'enabled', 'update', 'mid')}
                            for p in out['parts']]
        return out

    def on_install(self):
        e, part = self.selected()
        if not e:
            return
        entry = self.bare(e)
        if mods.is_manual(e):
            paths = ManualDialog(self, entry, part).result
            if paths:
                self.check_manual(entry, paths)
            return
        parts = None
        if e.get('parts'):
            if part:
                parts = [part['id']]
            else:
                parts = PartsDialog(self, e).result
                if not parts:
                    return
        size = sum(int(p['size']) for p in mods.parts_of(entry) if parts is None or p['id'] in parts)
        if e['kind'] == 'romfs' and not messagebox.askokcancel(
                self.cat('mods.title'), self.cat('mods.confirm_download', name=e['name'], url=e['homepage'],
                                                 size=max(1, size >> 20)), parent=self.win):
            return
        self.run(self.cat('mods.installing', name=e['name']),
                 lambda: (mods.install(self.dest, entry, self.progress, parts=parts), self.reload_now())[1])

    def check_manual(self, entry, paths):
        """A manual library entry's downloads: each matched to a part by SHA-256 and read, then installed as the
        library lists them, after a look at the layout only when mod_layout could not check it against the game's
        files. Picked files that are no part of the entry can be added as mods of their own."""
        def work():
            matched, unknown = mods.match_files(entry, paths)
            if not matched:
                mods.check_file(paths[0], entry)   # raises with the files the entry lists
            plans = {mid: mods.preview(self.dest, path) for mid, (_, path) in matched.items()}
            return matched, unknown, plans

        def then(value):
            matched, unknown, plans = value
            for mid, plan in plans.items():
                if not plan.sure and not self.confirm_layout(plan):
                    return
            files = [path for _, path in matched.values()]
            names = ', '.join(part['name'] for part, _ in matched.values())

            def install():
                mods.install(self.dest, entry, files=files, plans=plans)
                return self.reload_now()

            def done(listing):
                self.results.put(('ok', listing))
                if unknown and messagebox.askyesno(
                        self.cat('mods.title'), self.cat('mods.unknown_files', name=entry['name'],
                                                         files=', '.join(p.name for p in unknown)), parent=self.win):
                    self.add_archives(list(unknown))
            self.run(self.cat('mods.installing', name=names), install, done)
        self.run(self.cat('mods.checking', name=', '.join(Path(p).name for p in paths)), work, then)

    def add_archives(self, paths):
        """Adds each archive in turn (Add archive's steps), the next once the one before is done."""
        if paths:
            self.add_archive(paths[0], after=lambda: self.add_archives(paths[1:]))

    def on_toggle(self):
        e, part = self.selected()
        target = part or e
        if not target or not target['installed']:
            return
        on = not target['enabled']
        mid = part['mid'] if part else e['id']
        # one part containing another that is on is fine (the bdat merge takes a value two mods set alike once);
        # the line only says so
        both = mods.overlaps(e, mid) if on and part else []
        note = self.cat('mods.overlap_note', name=part['name'], others=', '.join(both)) if both else ''

        def done(listing):
            self.results.put(('ok', listing))
            if note:
                self.results.put(('ok', note))
        self.run('', lambda: (mods.set_enabled(self.dest, mid, on), self.reload_now())[1], done)

    def on_remove(self):
        e, part = self.selected()
        target = part or e
        if not target or not messagebox.askyesno(self.cat('mods.title'),
                                                 self.cat('mods.confirm_remove', name=target['name']), parent=self.win):
            return
        mid = part['mid'] if part else e['id']
        self.run('', lambda: (mods.remove(self.dest, mid), self.reload_now())[1])

    def on_add_zip(self):
        path = filedialog.askopenfilename(parent=self.win, title=self.cat('mods.add_zip'),
                                          filetypes=[(self.cat('mods.type_zip'), ('*.zip', '*.rar')),
                                                     (self.cat('mods.type_all'), '*')])
        if path:
            self.add_archive(path)

    def add_archive(self, path, top=None, after=None):
        """Reads the archive, shows its layout when it needs a look, then installs it as read; after() then."""
        def then(plan):
            if mod_layout.needs_confirm(plan) and not self.confirm_layout(plan):
                return after and after()

            def done(listing):
                self.results.put(('ok', listing))
                if after:
                    after()
            self.run(self.cat('mods.installing', name=Path(path).name),
                     lambda: (mods.add_archive(self.dest, path, plan=plan), self.reload_now())[1], done)

        def failed(error):
            if error.id != 'layout.several':
                return False
            choice = PickDialog(self, error.params['tops']).result
            if choice is not None:
                self.add_archive(path, choice, after)
            elif after:
                after()
            return True
        self.run(self.cat('mods.checking', name=Path(path).name), lambda: mods.preview(self.dest, path, top), then,
                 failed)

    def confirm_layout(self, plan):
        lines = '\n'.join(mod_layout.describe(plan, self.cat))
        return messagebox.askyesno(self.cat('mods.confirm_layout_title'),
                                   self.cat('mods.confirm_layout' if plan.sure else 'mods.confirm_layout_unsure',
                                            lines=lines), parent=self.win)

    def on_refresh(self):
        self.load()

    def on_page(self):
        e, _ = self.selected()
        if e and str(e.get('homepage', '')).startswith('https://'):
            webbrowser.open(e['homepage'])

    def on_close(self):
        self.win.destroy()

    def reload_now(self):
        """The list after a change, on the worker thread (the library entries stay as they were)."""
        index = [self.bare(e) for e in self.entries
                 if e.get('url') or e.get('files') or e.get('parts') or mods.is_manual(e)]
        return self.listed(index)

    def listed(self, index):
        """mods.status on the worker thread; the lines about mods put in the folder by hand go to the status line."""
        notes = []
        out = mods.status(self.dest, index, notes)
        if notes:
            self.results.put(('notes', notes))
        return out


class Dialog:
    """A small modal window over the Mods window: text, then buttons; result is what the pressed button set."""

    def __init__(self, owner, title):
        self.owner, self.result = owner, None
        w = self.win = tk.Toplevel(owner.win)
        w.title(title)
        w.transient(owner.win)
        w.resizable(False, False)
        w.configure(background=theme.BG)
        theme.dark_title_bar(w)
        self.body = ttk.Frame(w, padding=PAD)
        self.body.pack(fill='both', expand=True)
        w.protocol('WM_DELETE_WINDOW', self.close)
        w.bind('<Escape>', lambda _e: self.close())

    def show(self):
        w = self.win
        w.update_idletasks()
        parent = self.owner.win
        x = parent.winfo_rootx() + (parent.winfo_width() - w.winfo_reqwidth()) // 2
        y = parent.winfo_rooty() + (parent.winfo_height() - w.winfo_reqheight()) // 2
        w.geometry(f'+{max(0, x)}+{max(0, y)}')
        w.grab_set()
        w.wait_window()

    def close(self):
        self.win.grab_release()
        self.win.destroy()


class ManualDialog(Dialog):
    """How to get a mod whose site allows downloads only from its own page: the files to download there (each part
    of the entry, or the one part selected), a button that opens the page and one that picks the downloads.
    result: the picked files' paths, or None."""

    def __init__(self, owner, entry, part=None):
        cat = owner.cat
        super().__init__(owner, cat('mods.manual_title', name=entry['name']))
        self.entry = entry
        parts = [p for p in mods.parts_of(entry) if part is None or p['mid'] == part['mid']]
        self.pages = [p.get('page') for p in parts if str(p.get('page') or '').startswith('https://')]
        site = urlparse(self.pages[0] if self.pages else entry.get('homepage') or '').hostname or ''
        site = site[4:] if site.startswith('www.') else site
        size = lambda p: max(1, int(p['size']) >> 20)
        if len(parts) == 1:
            text = cat('mods.manual_intro', name=entry['name'], site=site or '?',
                       file=parts[0].get('file') or parts[0]['name'], size=size(parts[0]))
        else:
            files = '\n'.join(cat('mods.manual_part_line', part=p['name'], file=p.get('file') or '', size=size(p))
                              for p in parts)
            text = cat('mods.manual_intro_parts', name=entry['name'], site=site or '?', files=files)
        ttk.Label(self.body, text=text, wraplength=560, justify='left').pack(anchor='w')
        bar = ttk.Frame(self.body)
        bar.pack(fill='x', pady=(PAD, 0))
        ttk.Button(bar, text=cat('mods.manual_open'), command=self.on_open).pack(side='left')
        ttk.Button(bar, text=cat('mods.manual_pick' if len(parts) == 1 else 'mods.manual_pick_many'),
                   command=self.on_pick).pack(side='left', padx=(8, 0))
        ttk.Button(bar, text=cat('mods.cancel'), command=self.close).pack(side='right')
        self.show()

    def on_open(self):
        # the files of one Nexus mod share one page; open each distinct page once
        page = self.pages[0] if len(set(p.split('&file_id=')[0] for p in self.pages)) == 1 else None
        for url in ([page] if page else dict.fromkeys(self.pages)) or [self.entry.get('homepage') or '']:
            if url.startswith('https://'):
                webbrowser.open(url)

    def on_pick(self):
        kinds = sorted({'*' + Path(p.get('file') or '').suffix for p in mods.parts_of(self.entry)
                        if Path(p.get('file') or '').suffix} or {'*.zip', '*.rar'})
        paths = filedialog.askopenfilenames(parent=self.win, title=self.owner.cat('mods.manual_pick'),
                                            initialdir=str(Path.home() / 'Downloads'),
                                            filetypes=[(self.owner.cat('mods.type_zip'), tuple(kinds)),
                                                       (self.owner.cat('mods.type_all'), '*')])
        if paths:
            self.result = list(paths)
            self.close()


class PartsDialog(Dialog):
    """Which parts of an entry to download (entries whose parts the manager downloads itself). result: part ids."""

    def __init__(self, owner, entry):
        cat = owner.cat
        super().__init__(owner, cat('mods.parts_title', name=entry['name']))
        self.parts = entry['parts']
        ttk.Label(self.body, text=cat('mods.parts_pick'), wraplength=460, justify='left').pack(anchor='w')
        self.vars = []
        for p in self.parts:
            v = tk.BooleanVar(value=not p['installed'] and not any(p['id'] in q.get('includes', ()) for q in self.parts))
            ttk.Checkbutton(self.body, text=f"{p['name']} ({max(1, int(p['size']) >> 20)} MB)", variable=v).pack(
                anchor='w', pady=(4, 0))
            self.vars.append(v)
        bar = ttk.Frame(self.body)
        bar.pack(fill='x', pady=(PAD, 0))
        ttk.Button(bar, text=cat('mods.install'), command=self.on_ok).pack(side='left')
        ttk.Button(bar, text=cat('mods.cancel'), command=self.close).pack(side='right')
        self.show()

    def on_ok(self):
        self.result = [p['id'] for p, v in zip(self.parts, self.vars) if v.get()]
        self.close()


class PickDialog(Dialog):
    """Which of an archive's mod versions to install. result: the chosen folder, or None."""

    def __init__(self, owner, tops):
        cat = owner.cat
        super().__init__(owner, cat('mods.pick_top_title'))
        self.tops = list(tops)
        ttk.Label(self.body, text=cat('mods.pick_top'), wraplength=460, justify='left').pack(anchor='w')
        self.list = tk.Listbox(self.body, height=min(10, len(self.tops)), activestyle='none', exportselection=False,
                               background=theme.PANEL, foreground=theme.TEXT, selectbackground=theme.SELECTED,
                               selectforeground=theme.TEXT, highlightthickness=1, highlightbackground=theme.LINE,
                               relief='flat')
        for t in self.tops:
            self.list.insert('end', t)
        self.list.selection_set(0)
        self.list.pack(fill='x', pady=(8, 0))
        self.list.bind('<Double-Button-1>', lambda _e: self.on_ok())
        bar = ttk.Frame(self.body)
        bar.pack(fill='x', pady=(PAD, 0))
        ttk.Button(bar, text=cat('mods.pick_top_ok'), command=self.on_ok).pack(side='left')
        ttk.Button(bar, text=cat('mods.cancel'), command=self.close).pack(side='right')
        self.show()

    def on_ok(self):
        sel = self.list.curselection()
        if sel:
            self.result = self.tops[sel[0]]
            self.close()
