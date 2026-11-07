# Unmerged candidate integration control

This documentation-only commit is an intentional unmerged backend candidate
for the consuming shell's immutable-SHA integration gate. Its native sources,
fixture registration, workflows and producer contract are unchanged from
backend main `3855b96dff584c71a21a90c0631ddcfc062d6593`.

Do not merge this control. A successful consumer dispatch must report the exact
candidate SHA, preserve the shell's existing gitlink and lock, and remain
integration-only, never release-eligible. Independent missing/mismatched input,
real import-break and cancellation controls remain separate results.
