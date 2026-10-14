#!/usr/bin/env python3
"""Check two real Qwen turns and conversation switching through a full PTY.

Requires an already running local model. Owns its temporary C service, client
and checkpoints; existing model processes and user sessions are left intact.
"""
from pathlib import Path
import argparse
import json
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
from verify import Server, checkpoint
from verify_client_sessions import ManagedTTY, new_checkpoint
sys.path.insert(0, str(ROOT / "src/model"))
from transport import LocalServer, normalize_config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture', action='store_true', help='archive a full redraw of the actual model conversation')
    args = parser.parse_args()
    lines = ["$ python3 scripts/model/tui_smoke.py" + (" --capture" if args.capture else ""),
             "Actual all-stdio PTY; existing local Qwen model; isolated temporary sessions."]
    started = time.monotonic()
    try:
        config = normalize_config(json.loads((ROOT / "config/model.json").read_text()))
        model = LocalServer(config, timeout=5)
        assert model.json_request("/health").get("status") == "ok"
        lines.append(f"Healthy model endpoint: {model.endpoint}")
        with tempfile.TemporaryDirectory(prefix="task4-model-tui-") as temporary:
            home = Path(temporary)
            sessions = home / "clients"
            service = Server(home, model_config=str(ROOT / "config/model.json"))
            tty = None
            try:
                tty = ManagedTTY(service.port, sessions)
                first = new_checkpoint(tty, sessions)
                prompts = ["请记住代号 SPRUCE，只回答收到。", "请只回答刚才的代号。"]
                for number, prompt in enumerate(prompts, 1):
                    tty.send(prompt + "\r")
                    def finished():
                        assert "[conversation error]" not in tty.plain(), tty.visible()
                        value = checkpoint(first)
                        return value and value["request"] == number and value["done"] == 1 and value
                    saved = tty.wait(finished,
                                     f"real Qwen turn {number}", timeout=180)
                    turn = service.state / saved["token"] / f"turn-{number:08d}"
                    metrics = json.loads((turn / "metrics.json").read_text())
                    assert metrics["attempt_max_tokens"] == 4096, metrics
                    assert metrics["context_tokens"] == 16384, metrics
                    answer = saved["answer"].decode("utf-8")
                    if number == 2:
                        assert "SPRUCE" in answer, answer
                    lines.append(f"turn={number} done=1 answer={answer!r} "
                                 f"budget={metrics['attempt_max_tokens']} context={metrics['context_tokens']}")
                archive = Path(str(first) + ".history")
                tty.wait(lambda: len(list(archive.glob("turn-*.ckpt"))) == 2,
                         "both completed turns archived")
                if args.capture:
                    sys.path.insert(0, str(ROOT / 'scripts'))
                    from capture_conversation_ui import archive as capture_frame
                    tty.send('/thinking\r')
                    tty.wait(lambda: 'thinking hidden' in tty.visible(), 'hide thinking for the two-turn overview')
                    tty.send('下一问：请解释 fork 和 exec 的区别。')
                    capture_frame(tty, 'model-current-96x28', 96, 28,
                                  'Two actual Qwen turns recall SPRUCE; thinking is hidden locally and the next question remains unsent.', fixture=False)
                    tty.send(b'\x15')
                tty.send("/new\r")
                second = new_checkpoint(tty, sessions, [first])
                assert checkpoint(first)["token"] != checkpoint(second)["token"]
                tty.send("/switch " + first.parent.name + "\r")
                tty.wait(lambda: all(prompt in tty.visible() for prompt in prompts),
                         "both real prompts restored after switching")
                assert "SPRUCE" in tty.visible()
                tty.send("/quit\r")
                tty.finish()
                lines.extend(["PASS: actual Qwen completed two turns with 4096 output budget and 16k context.",
                              "PASS: both turns archived; /new and /switch restored the selected conversation.",
                              "PASS: clean TUI exit restored the original terminal settings."])
            finally:
                if tty is not None:
                    tty.close()
                service.close()
    except Exception as error:
        lines.append(f"FAIL: {error!r}")
        raise
    finally:
        lines.append(f"Elapsed: {time.monotonic() - started:.3f}s")
        (ROOT / "logs/model-tui-4096.txt").write_text("\n".join(lines) + "\n")
        print("\n".join(lines), flush=True)


if __name__ == "__main__":
    main()
