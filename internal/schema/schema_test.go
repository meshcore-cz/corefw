package schema

import "testing"

func f(v float64) *float64 { return &v }

func baseSchema() *Schema {
	return &Schema{
		Properties: map[string]Property{
			"low":  {Type: TypeInteger, Minimum: f(1), Maximum: f(99), Default: int64(30)},
			"crit": {Type: TypeInteger, Minimum: f(1), Maximum: f(99), Default: int64(15)},
			"name": {Type: TypeString, Default: "node"},
			"mode": {Type: TypeEnum, Enum: []string{"a", "b"}, Default: "a"},
			"iv":   {Type: TypeDuration, Default: "15min"},
		},
		Required: []string{"low", "crit"},
		Rules:    []Rule{{LessThan: [2]string{"crit", "low"}}},
	}
}

func TestValidateDefaults(t *testing.T) {
	out, err := baseSchema().Validate("c", map[string]any{})
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if out["low"] != int64(30) || out["crit"] != int64(15) {
		t.Fatalf("defaults not applied: %#v", out)
	}
	if out["name"] != "node" || out["mode"] != "a" || out["iv"] != "15min" {
		t.Fatalf("string/enum/duration defaults wrong: %#v", out)
	}
}

func TestValidateRangeError(t *testing.T) {
	_, err := baseSchema().Validate("c", map[string]any{"low": 200})
	if err == nil {
		t.Fatal("expected range error")
	}
}

func TestValidateRuleError(t *testing.T) {
	_, err := baseSchema().Validate("c", map[string]any{"low": 20, "crit": 25})
	if err == nil {
		t.Fatal("expected less_than rule violation")
	}
}

func TestValidateEnumError(t *testing.T) {
	_, err := baseSchema().Validate("c", map[string]any{"mode": "z"})
	if err == nil {
		t.Fatal("expected enum error")
	}
}

func TestValidateUnknownKey(t *testing.T) {
	_, err := baseSchema().Validate("c", map[string]any{"nope": 1})
	if err == nil {
		t.Fatal("expected unknown-key error")
	}
}

func TestValidateIntegerRejectsFloat(t *testing.T) {
	_, err := baseSchema().Validate("c", map[string]any{"low": 3.5})
	if err == nil {
		t.Fatal("expected non-integer error")
	}
}

func TestParseDuration(t *testing.T) {
	cases := map[string]int{"30s": 30, "15min": 900, "2h": 7200, "1d": 86400}
	for in, want := range cases {
		got, err := DurationSeconds(in)
		if err != nil || got != want {
			t.Fatalf("%s: got %d,%v want %d", in, got, err, want)
		}
	}
	if _, err := ParseDuration("5x"); err == nil {
		t.Fatal("expected bad-unit error")
	}
}
