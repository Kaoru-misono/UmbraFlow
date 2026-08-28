# Empty OCR decodes are not public text lines

Empty decoded text is not a useful text reading for callers. Omit those entries
from Framework screen reads, Runtime reads and the offline OCR command. If no
text remains, the existing absence shape applies: an empty line array, or
`text_found: false` for the single-line Tool. Do not trim nonempty strings or
discard low-confidence text; those decisions still belong to the Project.

This supersedes the empty-decoded-line distinction in
[the single-line Tool decision](2026-08-27-an-asserted-single-line-read-is-its-own-tool.md).
It changes neither layout selection nor the meaning of character confidence.

Native adapter observations still retain attempted boxes for cost accounting.
The cycle boundary charges all actual recognition work before filtering and
caches the filtered answer. Dropping an output must never refund an inference
or make a budget refusal look like a completed empty read. The CLI applies the
same output policy without changing detector or recognizer execution.

The permanent guards are the cycle budget test and the screen/CLI integration
tests; callers receive neither diagnostic empty boxes nor invented characters.
