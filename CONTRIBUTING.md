# Contributing

Thank you for taking the time to contribute.
To ensure nobody's time is wasted, please follow the rules below.

## Acceptable Code Contributions

- All changes submitted MUST be **fully understood by the submitter**. If you do not know why or how
  your change works, do not submit it to be merged. You must be able to explain your reasoning
  for every change.

- Change descriptions MUST NOT be written by AI. Summarize what you changed in your own words.
  If you think an AI summary of a chat session etc may provide useful context, include it in a spoiler.

- Changes MUST be submitted by a human who will be responsible for them. Changes submitted without
  a human in the loop such as automated tooling and AI Agents are **strictly disallowed**. Accounts
  responsible for such contribution attempts **will be banned**.

- Changes MUST respect Quickshell's license and the license of any source works. Changes including
  code from any other works must disclose the source of the code, explain why it was used, and
  ensure the license is compatible.

- Changes must follow the guidelines outlined in [HACKING.md](HACKING.md) for style and substance.

- Changes must stand on their own as a unit. Do not make multiple unrelated changes in one PR.
  Changes depending on prior merges should be marked as a draft.

## Build and validation

Use [BUILD.md](BUILD.md) for production configure, build and install commands,
including Windows dependencies, production targets and the public Windows
regression suite. `BUILD_TESTING` defaults to `OFF`; enable it on Windows to
build the tiered fixtures, then explicitly select the `portable` CTest label
for noninteractive validation. Other tiers must be selected deliberately.
Report the exact revision and platform checked, test counts and skipped cases,
what you verified, and anything not verified. Any separately maintained harness
results must correspond to that production revision.

## Acceptable Non-code Contributions

- Bug and crash reports. You must follow the instructions in the issue templates and provide the
  information requested.

- Feature requests can be made via Issues. Please check to ensure nobody else has requested the same feature.

- Do not make insubstantial or pointless changes.

- Changes to project rules / policy / governance will not be entertained, except from significant
  long-term contributors. These changes should not be addressed through contribution channels.

## Merge timelines

We handle work for the most part on a push basis. If your PR has been ignored for a while
and is still relevant please bump it.
