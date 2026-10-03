## C Library Requirements

- The library uses C23.
- The public API MUST NOT expose types defined by third-party libraries.
- Third-party types and headers MUST remain behind the library's private implementation boundary.
- Use opaque handles to represent internal objects whose implementation depends on third-party libraries.