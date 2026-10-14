#!/usr/bin/env python3
"""Capture actual client ANSI for conversation navigation and inline errors.

This deterministic fixture owns its sockets, PTYs and temporary checkpoints.
Answers are fixture text, not Qwen output. The resulting .ansi files must be
replayed verbatim with capture_terminal.py to obtain real xterm screenshots.
"""
import fcntl
from datetime import datetime
import json
from pathlib import Path
import socket
import struct
import sys
import tempfile
import termios
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from verify import HELLO, WELCOME, END, ERROR, frame, receive, checkpoint
from verify_controls import CANCEL, CANCELLED, THINK, question, emit
from verify_client_sessions import ManagedTTY, new_checkpoint, completed
from verify_tui import Screen, peer


def accept(listener, tty, old, token):
    sock, _ = listener.accept()
    sock.settimeout(6)
    assert receive(sock) == (HELLO, 0, 0, old)
    sock.sendall(frame(WELCOME, payload=token))
    tty.pause(0.12)
    return sock


def reply(sock, tty, cp, turn, prompt, answer):
    tty.send(prompt + '\r')
    assert question(sock, turn)[0] == prompt.encode()
    total = emit(sock, answer.encode(), request=turn)
    sock.sendall(frame(END, turn, total))
    completed(tty, cp, turn)
    tty.pause(0.15)


def archive(tty, stem, columns, rows, description, *, fixture=True):
    tty.drain()
    tty.screen = Screen(rows, columns)
    fcntl.ioctl(tty.slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, columns, 0, 0))
    tty.pause(0.2)
    mark = len(tty.output)
    # Diff rendering deliberately omits stable rows; explicit redraw produces
    # a standalone exact terminal frame instead of rebuilding it from text.
    tty.screen = Screen(rows, columns)
    tty.send(b'\x0c')
    tty.wait(lambda: 'TASK4' in tty.visible() and 'question' in tty.screen.lines()[-1], 'full evidence redraw')
    tty.pause(0.2)
    ansi = bytes(tty.output[mark:])
    (ROOT / 'logs' / (stem + '.ansi')).write_bytes(ansi)
    (ROOT / 'logs' / (stem + '.screen.txt')).write_text(tty.visible() + '\n')
    binary = ROOT / 'bin/stream_client'
    info = binary.stat()
    metadata = {'source': 'Deterministic UI fixture; not Qwen generation or model performance.' if fixture else 'Actual local Qwen generation through the current C client.',
        'captured_at': datetime.now(ZoneInfo('Asia/Shanghai')).isoformat(),
        'client_binary': str(binary), 'client_size': info.st_size, 'client_mtime_ns': info.st_mtime_ns,
        'method': 'Actual C client on an owned all-stdio PTY; Ctrl+L full redraw archived verbatim. Only the replay xterm will be screenshot.',
        'columns': columns, 'rows': rows, 'client_pid': tty.proc.pid,
        'client_alive': tty.proc.poll() is None, 'ansi_bytes': len(ansi),
        'description': description, 'draft': tty.screen.lines()[-1]}
    (ROOT / 'logs' / (stem + '.source.json')).write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(metadata, ensure_ascii=False), flush=True)


def run():
    with tempfile.TemporaryDirectory(prefix='task4-ui-conversations-') as temporary:
        sessions = Path(temporary) / 'clients'
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); listener.listen(3); listener.settimeout(6)
            tty = ManagedTTY(listener.getsockname()[1], sessions)
            sockets = []
            try:
                first_sock = accept(listener, tty, b'', b'a' * 32); sockets.append(first_sock)
                first = new_checkpoint(tty, sessions)
                reply(first_sock, tty, first, 1, '文件读取失败时应该怎么办？',
                      '[UI fixture] 先检查 fopen 返回值，再读取文件。')
                reply(first_sock, tty, first, 2, '读取结束后需要释放什么？',
                      '[UI fixture] 使用 fclose 关闭文件，保留错误检查。')
                tty.send('/new\r')
                second_sock = accept(listener, tty, b'', b'b' * 32); sockets.append(second_sock)
                second = new_checkpoint(tty, sessions, [first])
                reply(second_sock, tty, second, 1, '另一个独立的练习会话', '[UI fixture] 独立会话有自己的上下文和断点。')
                tty.send('/switch ' + first.parent.name + '\r')
                resumed = accept(listener, tty, b'a' * 32, b'a' * 32); sockets.append(resumed)
                tty.wait(lambda: '文件读取失败' in tty.visible() and '读取结束后' in tty.visible(), 'restored two-turn history')
                tty.send('/sessions\r')
                tty.wait(lambda: first.parent.name in tty.visible() and second.parent.name in tty.visible(), 'two local conversations listed')
                tty.send('/switch ' + second.parent.name)
                tty.pause(0.1)
                archive(tty, 'conversations-96x28', 96, 28,
                        'Two saved turns restored after switching back; /sessions lists both conversations; an unsent /switch command remains editable.')
                assert first.parent.name in tty.visible() and second.parent.name in tty.visible()
                assert '读取结束后' in tty.visible()
                tty.send(b'\x15/quit\r'); tty.finish()
            finally:
                tty.close()
                for sock in sockets: sock.close()

    with peer() as (sock, tty, cp):
        tty.send('请解释如何安全读取文件。\r')
        question(sock, 1)
        sock.sendall(frame(THINK, 1, 0, '[UI fixture] 正在组织文件读取步骤。'.encode()))
        tty.wait(lambda: '正在组织' in tty.visible(), 'thinking before correction')
        tty.send('/steer 只用一句话回答\r')
        kind, request, offset, payload = receive(sock)
        assert (kind, request, offset, payload) == (CANCEL, 1, 0, b'')
        sock.sendall(frame(CANCELLED, 1, 0))
        merged, _ = question(sock, 2)
        assert '只用一句话回答'.encode() in merged
        answer = '[UI fixture] 检查 fopen 的返回值，逐行读取后用 fclose 关闭文件。'
        total = emit(sock, answer.encode(), request=2)
        sock.sendall(frame(END, 2, total))
        completed(tty, cp, 2)
        tty.wait(lambda: 'cancelled' in tty.visible() and 'completed' in tty.visible(), 'cancelled and corrected turns')
        tty.send('下一问：读取失败时怎样报告错误？')
        archive(tty, 'controls-96x28', 96, 28,
                'A thinking turn is cancelled; the durable correction becomes a new completed turn; the next question remains editable.')
        tty.send(b'\x15/quit\r'); tty.finish()

    with peer() as (sock, tty, cp):
        tty.send('请解释如何恢复刚才的回答。\r')
        question(sock, 1)
        prefix = '[UI fixture] 已收到的正文会保留。'
        total = emit(sock, prefix.encode())
        sock.sendall(frame(ERROR, 1, total, b'fixture inference interrupted'))
        tty.wait(lambda: 'fixture inference interrupted' in tty.visible() and '/retry' in tty.visible(), 'recoverable inline error')
        assert tty.proc.poll() is None and checkpoint(cp)['done'] == 0
        tty.send('请改用两句话回答（未发送）')
        tty.pause(0.15)
        archive(tty, 'conversation-error-60x18', 60, 18,
                'Injected inference error stays inside the conversation; checkpoint remains pending and a Chinese correction is editable in the same living client process.')
        assert 'fixture inference interrupted' in tty.visible()
        assert '未发送' in tty.screen.lines()[-1]
        tty.send(b'\x15/quit\r'); tty.finish()


if __name__ == '__main__': run()
