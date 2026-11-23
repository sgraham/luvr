# D19 Formatter-friendly syntax choices

The constraints themselves are in [section 9](formatter.md). Two choices there affect the
grammar:

- **D19a: trailing commas.** Decided: allowed in list and dict literals
  only, not in calls, parameter lists or compound literals.
- **D19b: adjacent string literal concatenation** (`"abc" "def"`).
  Decided: **no**, because of the accidental missing-comma concatenation.
  Revisit with something explicit if long strings become a problem.
