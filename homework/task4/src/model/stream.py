"""Incremental separation of Qwen-style <think> markup from the final answer.

Only the opening reasoning block is interpreted. Literal markup in a final
answer is preserved. Pending marker prefixes stay in memory and in the raw
journal until later chunks disambiguate them.
"""

from transport import checked_text


class ThinkingParser:
    def __init__(self, thinking=True, initial_thinking=False):
        if initial_thinking and not thinking:
            raise ValueError("template opened <think> although thinking=false; check --jinja/template settings")
        self.mode = "thinking" if initial_thinking else ("prefix" if thinking else "final")
        self.pending = ""

    def feed(self, text):
        checked_text(text, "model content")
        self.pending += text
        if self.mode == "prefix":
            candidate = self.pending.lstrip()
            if candidate.startswith("<think>"):
                self.pending = candidate[len("<think>"):]
                self.mode = "thinking"
            elif not candidate or "<think>".startswith(candidate):
                return []
            else:
                self.mode = "final"
        if self.mode == "final":
            output, self.pending = self.pending, ""
            return [("final", output)] if output else []
        marker = "</think>"
        position = self.pending.find(marker)
        if position != -1:
            thought = self.pending[:position]
            answer = self.pending[position + len(marker):]
            self.pending = ""
            self.mode = "final"
            return [(kind, content) for kind, content in (("thinking", thought), ("final", answer)) if content]
        keep = 0
        for length in range(1, min(len(marker), len(self.pending) + 1)):
            if self.pending.endswith(marker[:length]):
                keep = length
        emit = self.pending[:-keep] if keep else self.pending
        self.pending = self.pending[-keep:] if keep else ""
        return [("thinking", emit)] if emit else []

    def finish(self):
        if self.mode == "thinking":
            raise ValueError("model stopped before </think>; raw checkpoint retained for continuation")
        text, self.pending = self.pending, ""
        self.mode = "final"
        return [("final", text)] if text else []
