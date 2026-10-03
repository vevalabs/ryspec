"""What the checks report: one reason a document is rejected."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Diagnostic:
    """One reason a document is rejected. `location` is a JSON path, or a
    `line:column` in the document, or empty when neither applies."""

    kind: str  # "io", "toml", "schema", and from libryspec "grammar" or "semantic"
    location: str
    message: str

    def __str__(self) -> str:
        where = f" {self.location}" if self.location else ""
        return f"{self.kind} error{where}: {self.message}"
