const encoder = new TextEncoder();
const maxQueued = 4096;

export class SharedTerminal {
    constructor(screen, status, wake) {
        this.screen = screen;
        this.status = status;
        this.wake = wake;
        this.keys = [];
        this.position = null;
    }

    async open() {
        const {Terminal} = await import("./xterm.mjs");
        this.terminal = new Terminal({
            cols: 30, rows: 20, disableStdin: true, cursorBlink: true, scrollback: 300,
            fontSize: 14, theme: {background: "#151f1c", foreground: "#e6eee9"},
        });
        this.terminal.open(this.screen);
        this.terminal.onData(text => {
            if (this.terminal.options.disableStdin) return;
            const bytes = encoder.encode(text);
            if (this.keys.length + bytes.length > maxQueued) {
                this.status.textContent = "Terminal input is full. Try a smaller paste.";
                return;
            }
            this.keys.push(...bytes);
            this.wake();
        });
    }

    stop() {
        this.keys.length = 0;
        if (this.terminal) this.terminal.options.disableStdin = true;
    }

    focus() {
        this.terminal?.focus();
    }

    async update(connection, boot) {
        if (!this.terminal) await this.open();
        if (this.boot !== boot) {
            this.stop();
            this.boot = boot;
            this.position = null;
        }
        // Remove a packet before sending it. An interrupted reply never retypes it.
        const input = Uint8Array.from(this.keys.splice(0, 128));
        const frame = await connection.exchangeTerminal(this.position, input);
        if (frame.reset) this.terminal.reset();
        if (this.terminal.cols !== frame.cols || this.terminal.rows !== frame.rows) {
            this.terminal.resize(frame.cols, frame.rows);
        }
        if (frame.bytes.length) {
            await new Promise(resolve => this.terminal.write(frame.bytes, resolve));
        }
        this.position = frame.position;
        this.terminal.options.disableStdin = false;
        this.status.textContent = "";
        return frame.more || this.keys.length > 0;
    }
}
