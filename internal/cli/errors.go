package cli

import (
	"errors"
	"fmt"
	"strings"
)

// UsageError marks invalid command usage or flags so ExecuteContext can map it
// to exit code 2 in one place.
type UsageError struct {
	Err error
}

func (e *UsageError) Error() string {
	return e.Err.Error()
}

func (e *UsageError) Unwrap() error {
	return e.Err
}

func usageErrorf(format string, args ...any) error {
	return &UsageError{Err: fmt.Errorf(format, args...)}
}

func isUsageError(err error) bool {
	var u *UsageError
	if errors.As(err, &u) {
		return true
	}
	msg := err.Error()
	return strings.Contains(msg, "unknown command") ||
		strings.Contains(msg, "unknown flag") ||
		strings.Contains(msg, "accepts ") ||
		strings.Contains(msg, "requires ") ||
		strings.Contains(msg, "invalid argument")
}
