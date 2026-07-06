package schema

import (
	"fmt"
	"strconv"
	"strings"
	"time"
)

// ParseDuration parses the compact, declarative duration form used in profiles
// and schemas: an integer followed by a unit suffix (s, m/min, h, d). This is
// intentionally stricter and more readable than Go's time.ParseDuration (which
// has no day unit and accepts fractional forms we don't want in config).
func ParseDuration(s string) (time.Duration, error) {
	s = strings.TrimSpace(s)
	if s == "" {
		return 0, fmt.Errorf("empty duration")
	}
	// Split into leading digits and trailing unit.
	i := 0
	for i < len(s) && (s[i] >= '0' && s[i] <= '9') {
		i++
	}
	if i == 0 {
		return 0, fmt.Errorf("invalid duration %q: expected a number", s)
	}
	n, err := strconv.Atoi(s[:i])
	if err != nil {
		return 0, fmt.Errorf("invalid duration %q: %v", s, err)
	}
	unit := strings.ToLower(strings.TrimSpace(s[i:]))
	switch unit {
	case "s", "sec", "secs":
		return time.Duration(n) * time.Second, nil
	case "m", "min", "mins":
		return time.Duration(n) * time.Minute, nil
	case "h", "hr", "hour", "hours":
		return time.Duration(n) * time.Hour, nil
	case "d", "day", "days":
		return time.Duration(n) * 24 * time.Hour, nil
	default:
		return 0, fmt.Errorf("invalid duration %q: unknown unit %q (use s/min/h/d)", s, unit)
	}
}

// DurationSeconds returns the total whole seconds of a compact duration, for
// emitting into generated C++ as an integer.
func DurationSeconds(s string) (int, error) {
	d, err := ParseDuration(s)
	if err != nil {
		return 0, err
	}
	return int(d / time.Second), nil
}
