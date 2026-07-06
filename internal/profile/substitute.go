package profile

import (
	"fmt"
	"regexp"
	"strings"
)

var varPattern = regexp.MustCompile(`\$\{([a-zA-Z_][a-zA-Z0-9_]*)\}`)

// substitute replaces ${name} references with the corresponding variable value.
// It is intentionally simple and declarative — no arbitrary expressions — so
// configuration stays validatable and reproducible. An undefined reference is a
// hard error rather than an empty expansion, to catch typos early.
func substitute(text string, vars map[string]any) (string, error) {
	var missing []string
	out := varPattern.ReplaceAllStringFunc(text, func(m string) string {
		name := varPattern.FindStringSubmatch(m)[1]
		v, ok := vars[name]
		if !ok {
			missing = append(missing, name)
			return m
		}
		return scalarToString(v)
	})
	if len(missing) > 0 {
		return "", fmt.Errorf("undefined variable(s): %s", strings.Join(dedup(missing), ", "))
	}
	return out, nil
}

func scalarToString(v any) string {
	switch t := v.(type) {
	case string:
		return t
	case bool:
		if t {
			return "true"
		}
		return "false"
	default:
		return fmt.Sprintf("%v", t)
	}
}

func dedup(in []string) []string {
	seen := map[string]bool{}
	var out []string
	for _, s := range in {
		if !seen[s] {
			seen[s] = true
			out = append(out, s)
		}
	}
	return out
}
