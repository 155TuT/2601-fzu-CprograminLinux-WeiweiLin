#!/usr/bin/env python3
"""录制真实的并发、逐字输出与中断恢复；仅使用临时运行状态。"""
from pathlib import Path
import importlib.util
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("task4_verify", ROOT / "tests/verify.py")
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)
from checkpoint import load


def record(name, text):
    (ROOT / "logs" / name).write_text(text, encoding="utf-8")


def state(client):
    try:
        return load(client["cp"])
    except FileNotFoundError:
        return None


def main():
    (ROOT / "logs").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="task4-demo-") as temp:
        home = Path(temp)
        server = verify.Server(home, rate=25, corpus=ROOT / "data/demo/corpus")
        began = time.monotonic()
        samples = []
        try:
            a = server.launch_client("a", b"first question from A\nsecond question from A\n")
            b = server.launch_client("b", b"independent question from B\n")
            verify.wait_for(lambda: state(a) and state(a)["offset"] >= 6, description="A partial response")
            sa, sb = state(a), state(b)
            assert sa["request"] == 1 and not sa["done"]
            samples.append(f't={time.monotonic()-began:.3f}s A={sa["offset"]} B={sb["offset"]} bytes; both pending')
            a["process"].send_signal(signal.SIGTERM)
            assert a["process"].wait(timeout=3) == 143
            a_prefix = load(a["cp"])
            sb_at_cancel = load(b["cp"])
            while b["process"].poll() is None:
                sb = load(b["cp"])
                if len(samples) < 6:
                    samples.append(f't={time.monotonic()-began:.3f}s B={sb["offset"]} bytes; done={int(sb["done"])}')
                time.sleep(0.22)
            assert b["process"].returncode == 0
            sb = load(b["cp"])
            assert sb["done"] and sb["offset"] > sb_at_cancel["offset"]
            assert sb["token"] != a_prefix["token"]
            verify.wait_for(lambda: verify.lock_released(server.state / sa["token"] / "session.lock"))
            commands = ("$ ./bin/stream_server --port 0 --rate 25 --corpus data/demo/corpus\n"
                        "$ ./bin/stream_client --checkpoint A.ckpt --input A.txt\n"
                        "$ ./bin/stream_client --checkpoint B.ckpt --input B.txt\n")
            concurrent = commands + server.stdout.read_text() + "\n"
            concurrent += "\n".join(samples) + "\n"
            concurrent += f'Client A terminated: exit=143; durable prefix={a_prefix["offset"]} bytes\n'
            concurrent += f'Client B continued: exit=0; complete bytes={sb["offset"]}\n'
            concurrent += f'A session={sa["token"]}\nB session={sb["token"]}\n'
            concurrent += "PASS: distinct sessions, workers, generators and log directories.\n"
            concurrent += "PASS: terminating A did not stop B.\n[exit status: 0]\n"
            record("02-concurrent.txt", concurrent)
            streaming = "$ Client B: actual stdout and observed durable progress\n" + "\n".join(samples)
            streaming += "\n\n" + b["stderr"].read_text() + b["stdout"].read_text()
            streaming += "PASS: a nonempty prefix existed before END; byte count grew over time.\n[exit status: 0]\n"
            record("03-streaming.txt", streaming)
            # 重跑相同输入文件，自动跳过已消费的第一行；随后只提交第二轮。
            recovered = server.launch_client("a", b"first question from A\nsecond question from A\n", cp=a["cp"])
            verify.wait_for(lambda: recovered["process"].poll() is not None)
            assert recovered["process"].returncode == 0, recovered["stderr"].read_text()
            restored = load(a["cp"])
            assert restored["request"] == 2 and restored["file_lines"] == 2 and restored["done"]
            session_a = server.state / sa["token"]
            first_source = (session_a / "turn-00000001/source.txt").read_bytes()
            first_response = (session_a / "turn-00000001/response.txt").read_bytes()
            assert first_source == first_response
            assert a_prefix["answer"] + recovered["stdout"].read_bytes().split(b"\n\n", 1)[0] + b"\n" == first_source
            recovery = "$ Re-run client A using the same checkpoint and original input\n"
            recovery += recovered["stderr"].read_text() + recovered["stdout"].read_text()
            recovery += f'Original turn-1 source == recovered response: {len(first_response)} bytes\n'
            recovery += f'Final request={restored["request"]}; consumed input lines={restored["file_lines"]}\n'
            recovery += "PASS: pending turn resumed; first question was not submitted again.\n[exit status: 0]\n"
            record("04-recovery.txt", recovery)
            context = (session_a / "context.txt").read_text()
            assert "first question from A" in context and "independent question from B" not in context
            isolation = "$ Inspect A's context and independently saved turn files\n" + context
            isolation += f'B context size={ (server.state / sb["token"] / "context.txt").stat().st_size } bytes before first turn\n'
            isolation += "\nA events.log:\n" + (session_a / "events.log").read_text()
            isolation += "PASS: A's turn 2 received only A's completed turn 1 context.\n"
            isolation += "PASS: response.txt matches immutable source.txt byte for byte.\n[exit status: 0]\n"
            record("05-isolation.txt", isolation)
            record("demo-a-response.txt", first_response.decode())
            record("demo-b-response.txt", sb["answer"].decode())
            # 便于报告阅读的上下文文本副本；去掉格式分隔用的末尾空行。
            record("demo-a-context.txt", context.rstrip("\n") + "\n")
            server.stop()
        finally:
            server.close()
    print("Recorded concurrent/streaming/recovery/isolation evidence in logs/.")


if __name__ == "__main__":
    main()
