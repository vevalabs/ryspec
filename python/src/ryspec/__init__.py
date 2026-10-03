"""Validate ryspec documents against the ryspec JSON Schema."""

from ryspec.diagnostic import Diagnostic


def __getattr__(name: str):
    # validate_file loads jsonschema, which `ryspec lint` does without.
    if name == "validate_file":
        from ryspec.validate import validate_file

        return validate_file
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")


__all__ = ["Diagnostic", "validate_file"]
