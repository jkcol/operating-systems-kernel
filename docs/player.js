// A small ANSI terminal and asciicast player.
//
// The casts in casts/ are asciicast v2: a JSON header line followed by one
// [time, "o", data] array per line. Rogue is the demanding one -- it addresses
// the cursor and erases regions -- so the terminal implements CUP/ED/EL/SGR
// and the cursor moves, which is everything these four recordings use.

const COLORS = [
    '#1c2029', '#e05561', '#8cc265', '#d18f52', '#4aa5f0', '#c162de', '#42b3c2', '#c4c7cc',
    '#3e4451', '#ff616e', '#a5e075', '#f0a45d', '#4dc4ff', '#de73ff', '#4cd1e0', '#e6e6e6'
];

class Term {
    constructor(cols, rows) {
        this.cols = cols;
        this.rows = rows;
        this.reset();
    }

    reset() {
        this.grid = [];
        for (let y = 0; y < this.rows; y++) this.grid.push(this.blankRow());
        this.x = 0;
        this.y = 0;
        this.sgr = {fg: -1, bg: -1, bold: false, rev: false};
        this.pending = '';
        this.dirty = true;
    }

    blankRow() {
        const row = [];
        for (let x = 0; x < this.cols; x++) row.push({c: ' ', fg: -1, bg: -1, bold: false, rev: false});
        return row;
    }

    cell() {
        return {c: ' ', fg: this.sgr.fg, bg: this.sgr.bg, bold: this.sgr.bold, rev: this.sgr.rev};
    }

    write(chunk) {
        const s = this.pending + chunk;
        let i = 0;

        while (i < s.length) {
            const ch = s[i];

            if (ch === '\x1b') {
                if (i + 1 >= s.length) break;                  // sequence split across chunks
                const next = s[i + 1];

                if (next === '[') {
                    let j = i + 2;
                    while (j < s.length && !(s[j] >= '@' && s[j] <= '~')) j++;
                    if (j >= s.length) break;
                    this.csi(s.slice(i + 2, j), s[j]);
                    i = j + 1;
                    continue;
                }
                if (next === ']') {                            // OSC, runs to BEL or ST
                    let j = i + 2;
                    while (j < s.length && s[j] !== '\x07' && s[j] !== '\x1b') j++;
                    if (j >= s.length) break;
                    i = (s[j] === '\x1b') ? j + 2 : j + 1;
                    continue;
                }
                if ('()#%'.includes(next)) {                   // charset selection
                    if (i + 2 >= s.length) break;
                    i += 3;
                    continue;
                }
                i += 2;                                        // ESC 7, ESC 8, ESC M ...
                continue;
            }

            switch (ch) {
                case '\r': this.x = 0; break;
                case '\n': this.newline(); break;
                case '\b': this.x = Math.max(0, this.x - 1); break;
                case '\t': this.x = Math.min(this.cols - 1, (this.x + 8) & ~7); break;
                case '\x07': break;
                default:
                    if (ch >= ' ') this.put(ch);
            }
            i++;
        }

        this.pending = s.slice(i);
        this.dirty = true;
    }

    put(ch) {
        if (this.x >= this.cols) { this.x = 0; this.newline(); }
        const cell = this.cell();
        cell.c = ch;
        this.grid[this.y][this.x] = cell;
        this.x++;
    }

    newline() {
        this.y++;
        if (this.y >= this.rows) {
            this.grid.shift();
            this.grid.push(this.blankRow());
            this.y = this.rows - 1;
        }
    }

    csi(params, final) {
        const raw = params.replace(/^\?/, '');
        const p = raw.split(';').map((v) => (v === '' ? 0 : parseInt(v, 10) || 0));
        const n = Math.max(1, p[0] || 0);

        switch (final) {
            case 'H': case 'f':
                this.y = (p[0] || 1) - 1;
                this.x = (p[1] || 1) - 1;
                break;
            case 'A': this.y -= n; break;
            case 'B': this.y += n; break;
            case 'C': this.x += n; break;
            case 'D': this.x -= n; break;
            case 'G': this.x = (p[0] || 1) - 1; break;
            case 'd': this.y = (p[0] || 1) - 1; break;
            case 'J': this.eraseDisplay(p[0] || 0); break;
            case 'K': this.eraseLine(p[0] || 0); break;
            case 'm': this.applySgr(raw === '' ? [0] : p); break;
            default: break;                                    // modes, scroll regions: ignored
        }

        this.x = Math.min(this.cols - 1, Math.max(0, this.x));
        this.y = Math.min(this.rows - 1, Math.max(0, this.y));
    }

    eraseDisplay(mode) {
        if (mode === 2) {
            for (let y = 0; y < this.rows; y++) this.grid[y] = this.blankRow();
            return;
        }
        if (mode === 0) {
            this.eraseLine(0);
            for (let y = this.y + 1; y < this.rows; y++) this.grid[y] = this.blankRow();
        } else {
            this.eraseLine(1);
            for (let y = 0; y < this.y; y++) this.grid[y] = this.blankRow();
        }
    }

    eraseLine(mode) {
        const from = mode === 0 ? this.x : 0;
        const to = mode === 1 ? this.x : this.cols - 1;
        for (let x = from; x <= to; x++) this.grid[this.y][x] = this.cell();
    }

    applySgr(p) {
        for (let i = 0; i < p.length; i++) {
            const v = p[i];
            if (v === 0) this.sgr = {fg: -1, bg: -1, bold: false, rev: false};
            else if (v === 1) this.sgr.bold = true;
            else if (v === 7) this.sgr.rev = true;
            else if (v === 22) this.sgr.bold = false;
            else if (v === 27) this.sgr.rev = false;
            else if (v >= 30 && v <= 37) this.sgr.fg = v - 30;
            else if (v === 39) this.sgr.fg = -1;
            else if (v >= 40 && v <= 47) this.sgr.bg = v - 40;
            else if (v === 49) this.sgr.bg = -1;
            else if (v >= 90 && v <= 97) this.sgr.fg = v - 90 + 8;
            else if (v >= 100 && v <= 107) this.sgr.bg = v - 100 + 8;
        }
    }

    // Runs of identically styled cells become one span.
    toHTML(showCursor) {
        const esc = (c) => (c === '&' ? '&amp;' : c === '<' ? '&lt;' : c === '>' ? '&gt;' : c);
        const style = (cell, cursor) => {
            let fg = cell.fg, bg = cell.bg;
            if (cell.rev) { const t = fg; fg = bg === -1 ? 0 : bg; bg = t === -1 ? 15 : t; }
            if (cursor) { const t = fg; fg = bg === -1 ? 0 : bg; bg = t === -1 ? 15 : t; }
            const parts = [];
            if (fg !== -1) parts.push('color:' + COLORS[cell.bold && fg < 8 ? fg + 8 : fg]);
            if (bg !== -1) parts.push('background:' + COLORS[bg]);
            if (cell.bold) parts.push('font-weight:700');
            return parts.join(';');
        };

        let html = '';
        for (let y = 0; y < this.rows; y++) {
            const row = this.grid[y];
            let run = '', runStyle = null;
            for (let x = 0; x < this.cols; x++) {
                const isCursor = showCursor && y === this.y && x === this.x;
                const st = style(row[x], isCursor);
                if (st !== runStyle) {
                    if (run) html += runStyle ? `<span style="${runStyle}">${run}</span>` : run;
                    run = '';
                    runStyle = st;
                }
                run += esc(row[x].c);
            }
            if (run) html += runStyle ? `<span style="${runStyle}">${run}</span>` : run;
            html += '\n';
        }
        return html;
    }
}

class CastPlayer {
    constructor(screen, progress) {
        this.screen = screen;
        this.progress = progress;
        this.term = new Term(100, 30);
        this.events = [];
        this.speed = 1;
        this.endPause = 2.5;
        this.playing = false;
        this.raf = null;
        this.blink = 0;
    }

    async load(url, speed) {
        const text = await (await fetch(url)).text();
        const lines = text.split('\n').filter((l) => l.trim() !== '');
        const header = JSON.parse(lines[0]);
        this.term = new Term(header.width || 100, header.height || 30);
        this.events = lines.slice(1).map((l) => JSON.parse(l)).filter((e) => e[1] === 'o');
        this.duration = this.events.length ? this.events[this.events.length - 1][0] : 0;
        this.speed = speed || 1;
        this.restart();
    }

    restart() {
        this.term.reset();
        this.index = 0;
        this.startedAt = performance.now();
        this.render();
    }

    play() {
        if (this.playing) return;
        this.playing = true;
        // Resume where we left off rather than jumping back to the start.
        this.startedAt = performance.now() - (this.elapsed || 0) * 1000 / this.speed;
        this.loop();
    }

    pause() {
        this.playing = false;
        if (this.raf) cancelAnimationFrame(this.raf);
        this.raf = null;
    }

    loop() {
        if (!this.playing) return;
        const t = ((performance.now() - this.startedAt) / 1000) * this.speed;
        this.elapsed = t;

        while (this.index < this.events.length && this.events[this.index][0] <= t) {
            this.term.write(this.events[this.index][2]);
            this.index++;
        }

        if (t > this.duration + this.endPause) {
            this.elapsed = 0;
            this.restart();
        } else {
            this.render();
        }

        this.raf = requestAnimationFrame(() => this.loop());
    }

    render() {
        this.blink = (this.blink + 1) % 60;
        this.screen.innerHTML = this.term.toHTML(this.blink < 35);
        if (this.progress && this.duration) {
            const pct = Math.min(100, ((this.elapsed || 0) / this.duration) * 100);
            this.progress.style.width = pct + '%';
        }
    }
}

const DEMOS = [
    {
        id: 'trek',
        tab: 'trek',
        blurb: 'Star Trek, 1970s BSD games. Scans, damage report, shield control, and the ' +
               'library computer &mdash; all of it a user-mode process talking to the kernel ' +
               'through read and write syscalls.'
    },
    {
        id: 'rogue',
        tab: 'rogue',
        blurb: 'Rogue draws a full screen with ANSI cursor addressing and reads unbuffered ' +
               'single keystrokes, which leans on the UART driver and the terminal I/O path ' +
               'harder than anything else here.'
    },
    {
        id: 'zork',
        tab: 'zork',
        blurb: 'Zork reads its 133&thinsp;KB data file back off KTFS while it plays, so every ' +
               'turn goes through the filesystem, the write-back block cache, and the VirtIO ' +
               'block driver.'
    },
    {
        id: 'hello',
        tab: 'boot + fork',
        speed: 0.05,
        blurb: 'The whole boot: page tables, device probe, filesystem mount, then a user ' +
               'process that forks, waits on its child, and prints from both sides. ' +
               '<strong>Slowed 20&times;</strong> &mdash; in real time the entire sequence ' +
               'takes about 60&thinsp;ms.'
    }
];

document.addEventListener('DOMContentLoaded', () => {
    const screen = document.getElementById('screen');
    const progress = document.getElementById('progress');
    const tabs = document.getElementById('tabs');
    const blurb = document.getElementById('blurb');
    const toggle = document.getElementById('toggle');
    const player = new CastPlayer(screen, progress);

    const select = async (demo, button) => {
        [...tabs.children].forEach((b) => b.classList.toggle('active', b === button));
        blurb.innerHTML = demo.blurb;
        player.pause();
        player.elapsed = 0;
        await player.load(`casts/${demo.id}.cast`, demo.speed);
        player.play();
        toggle.textContent = 'pause';
    };

    DEMOS.forEach((demo, i) => {
        const button = document.createElement('button');
        button.textContent = demo.tab;
        button.onclick = () => select(demo, button);
        tabs.appendChild(button);
        if (i === 0) select(demo, button);
    });

    toggle.onclick = () => {
        if (player.playing) { player.pause(); toggle.textContent = 'play'; }
        else { player.play(); toggle.textContent = 'pause'; }
    };

    // Don't burn cycles animating a terminal nobody is looking at.
    document.addEventListener('visibilitychange', () => {
        if (document.hidden) player.pause();
        else if (toggle.textContent === 'pause') player.play();
    });
});
