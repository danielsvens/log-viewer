# Query language

Filters have three modes: plain text, regular expressions, and JSON field
conditions. The prefix determines the mode; this is a small custom language,
not SQL, jq, or a shell pipeline.

## Entering a query

In the TUI, press `/` and type a query. Results update while you type. Enter
keeps the filter; Esc restores the previous filter. Press `c` outside the editor
to clear it. An empty query shows every record in the selected source scope.

The source tree chooses where to search: one file or **All sources**. Changing
sources keeps the current query. Searches include the complete indexed history
and newly arriving records, rather than only the visible screen.

From the command line, quote the whole expression so the shell passes it as one
argument:

```sh
./log-viewer --filter 'error AND timeout' ./logs/
./log-viewer --dump --filter 'json | http.status >= 500' app.log worker.log
```

`--dump` prints matches and exits after the current inputs reach EOF. In the
examples below, the initial `/` used to open the TUI editor is not part of the
query itself.

## Plain text

```text
error
connection refused
error AND timeout AND payment
```

Plain text searches the entire record as a literal substring, including JSON
keys and their raw string representations. It does not require a whole-word
match: `error` also matches `errors`. ASCII letters are case-insensitive, so
`ERROR` and `error` match the same text. Unicode case folding is not performed.

Without ` AND `, the entire query is one phrase. Spaces, leading/trailing
whitespace, quotes, punctuation, and regex characters are literal. For example,
`connection refused` requires that phrase, and `"error"` includes the quotes.

The exact uppercase separator ` AND ` requires every term to occur in the same
record, in any order. Whitespace around each term is trimmed in this mode.
`error and timeout` is a literal phrase, not a conjunction. Terms must be
nonempty, and at most 32 terms are allowed. There is no quoting or escaping to
turn the uppercase separator into literal text; use regex when needed.

Plain text does not interpret `OR`, `NOT`, parentheses, or comparison operators.
For alternatives, use regex mode.

## Regular expressions

Start the query with `~`:

```text
~error|warn
~^ERROR
~timeout.*payment
~status[=: ]+5[0-9]{2}
```

The remaining text is a POSIX extended regular expression, matched
case-insensitively. A match anywhere in the record is enough unless anchored
with `^` or `$`.

| Pattern | Meaning |
| --- | --- |
| `error|warn` | Either alternative |
| `^ERROR` | Starts with ERROR |
| `done$` | Ends with done |
| `timeout.*payment` | Timeout followed by payment |
| `[0-9]+` | One or more digits |
| `[[:space:]]` | A POSIX whitespace character class |

Use POSIX classes such as `[[:digit:]]` or `[0-9]` rather than relying on
PCRE-style escapes such as `\d`. Lookarounds and lazy quantifiers are not
supported POSIX ERE syntax. Case-insensitive matching beyond ASCII depends on
the active locale and the system regex implementation.

` AND ` has no special meaning in regex mode. Write the whole expression as a
regex, or use plain-text mode when you need unordered AND terms. Invalid regex
syntax displays an error; it does not fall back to a literal search.

## JSON field conditions

Use the exact lowercase prefix `json |` at the beginning:

```text
json | level == "error"
json | http.status >= 500
json | message ~ "timeout"
json | healthy == false
json | context == null
```

The form is:

```text
json | field.path operator value
```

The `|` is a syntax marker, not a general pipeline. Each record must be a valid
JSON document. A missing field or invalid JSON does not match, including for
`!=`. JSON field names and value comparisons are case-sensitive.

Dot-separated paths traverse object keys: `http.status` reads `status` inside
`http`. Field paths accept letters, digits, underscores, hyphens, and dots.
Quoted keys, keys containing literal dots, and array indexing such as
`items[0].status` are not supported.

| Operator | Behavior |
| --- | --- |
| `==` | Exact textual equality |
| `!=` | Text differs, and the field exists |
| `~` | Case-sensitive literal substring |
| `>`, `>=`, `<`, `<=` | Numeric comparison; the field must be a JSON number |

Quote string values with JSON double quotes. JSON escapes such as `\"`, `\n`,
and `\u0065` are decoded. Unquoted values are accepted too; `true`, `false`,
`null`, and numbers are convenient examples. Single quotes are shell quoting in
the command-line examples, not JSON string syntax inside the query.

Equality is textual, not type-strict or numeric equality. Both `{"status":503}`
and `{"status":"503"}` match `json | status == 503`. The number `503.0` does
not match that equality because its text differs. Similarly, `true` and the
string `"true"` compare equally. Object/array fields compare using their
minified JSON representation. Prefer scalar fields for predictable queries.

Ordering comparisons behave differently: `json | status >= 500` matches
`{"status":503}`, but not `{"status":"503"}`. Numeric comparisons use
floating-point values, so very large integers may lose precision.

### Chaining conditions

Use uppercase `AND` and `OR` after the initial `json |` prefix:

```text
json | message ~ "error" AND message ~ "500"
json | level == "error" AND http.status >= 500
json | level == "error" OR level == "warn"
json | (level == "error" OR level == "warn") AND http.status >= 500
```

`AND` requires both sides; `OR` requires either side. Conditions are evaluated
against the same JSON record, with short-circuit evaluation. `AND` binds more
tightly than `OR`: `a == 1 OR b == 2 AND c == 3` means
`a == 1 OR (b == 2 AND c == 3)`. Parentheses override precedence.

The first example requires the decoded `message` field to include both `error`
and `500`, in either order. JSON `~` is a **case-sensitive literal substring**
operator, not a regular expression. The old `contains` spelling is rejected.
A leading `~` outside JSON mode still selects whole-record regex matching.

Quoted values can include spaces, `AND`, `OR`, and parentheses without acting as
syntax: `json | message ~ "error AND (500)"` searches for that literal text.
Unquoted values are a single token; quote values containing whitespace or
parentheses. Lowercase `and`/`or`, dangling connectors, unmatched parentheses,
and trailing unparsed text are errors. Repeat the field in each condition.

A missing field makes its condition false, including for `!=`; another branch
of an `OR` may still match. An invalid JSON document never matches the chain.
Up to **32 conditions** and **32 nested pairs of parentheses** are supported,
subject to the overall query length limit. Additional pipeline stages, `NOT`,
and mixing whole-record text conditions into JSON chains are not supported.

## Limits and matching details

- A complete query may contain at most **511 bytes**, including its prefix.
- Plain text supports at most **32 AND terms**.
- JSON paths may contain at most **127 bytes**. The supplied JSON value must be
  shorter than **256 bytes** before decoding; decoded strings must also fit.
- JSON documents beyond **128 nesting levels** or the **16,384 structural-item
  budget** are treated as nonmatches for JSON queries.
- A record is one input line. Terms and conditions do not span separate records.
- ANSI CSI color sequences and control characters are stripped before matching;
  tabs become spaces. JSON mode decodes JSON strings, while text/regex modes
  search the normalized record text, including its JSON escape spelling.
- Mode prefixes must start at the first byte. Leading spaces before `~` or
  `json |` make the query plain text instead.

Syntax errors are shown in the TUI with no matching results. Invalid initial
`--filter` expressions exit with status 2. A valid query with no matches is not
an error.
