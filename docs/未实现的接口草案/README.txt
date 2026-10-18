These headers were produced by subagents that ran out of context BEFORE writing
the accompanying .cpp implementations. Nothing includes them and nothing
compiles them; they were moved here so that no future reader mistakes them for
working code.

They are kept only as an interface sketch. If the work resumes, read the header,
delete it, and write the implementation as one step -- the previous attempt
failed precisely because the design and the implementation were split across two
budgets.

See the main docs (docs/README.md) for what is actually implemented and verified.
