## Requirements

- Every constraint defined by the schema MUST have at least one corresponding test case in the test corpus.
- Tests MUST exercise both valid and invalid cases where the constraint distinguishes between accepted and rejected input.
- When a schema constraint is added or modified, the corresponding corpus tests MUST be added or updated in the same change.
- Tests MUST verify the constraint's semantics rather than merely exercising the affected schema element.