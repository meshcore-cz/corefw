package profile

import "testing"

func TestParseSubstitution(t *testing.T) {
	data := []byte(`
name: demo
variables:
  region: eu868
  nm: "My Node"
board: heltec-v3
modules:
  - type: repeater
    advert_name: ${nm}
radio:
  region: ${region}
`)
	p, err := Parse(data)
	if err != nil {
		t.Fatal(err)
	}
	if p.Radio["region"] != "eu868" {
		t.Fatalf("region substitution failed: %v", p.Radio["region"])
	}
	if len(p.Modules) != 1 || p.Modules[0].ID != "repeater" {
		t.Fatalf("module parse failed: %#v", p.Modules)
	}
	if got := p.Modules[0].Options["advert_name"]; got != "My Node" {
		t.Fatalf("nested substitution failed: %v", got)
	}
}

func TestParseMissingVariable(t *testing.T) {
	_, err := Parse([]byte("board: x\nradio:\n  region: ${nope}\n"))
	if err == nil {
		t.Fatal("expected undefined-variable error")
	}
}

func TestComponentRefForms(t *testing.T) {
	p, err := Parse([]byte(`
board: heltec-v3
modules:
  - repeater
  - type: companion
    transport: usb
  - room-server:
      admin_password: x
`))
	if err != nil {
		t.Fatal(err)
	}
	if p.Modules[0].ID != "repeater" {
		t.Fatalf("scalar form failed: %#v", p.Modules[0])
	}
	if p.Modules[1].ID != "companion" || p.Modules[1].Options["transport"] != "usb" {
		t.Fatalf("type form failed: %#v", p.Modules[1])
	}
	if p.Modules[2].ID != "room-server" || p.Modules[2].Options["admin_password"] != "x" {
		t.Fatalf("single-key form failed: %#v", p.Modules[2])
	}
}

func TestSourceShorthand(t *testing.T) {
	p, err := Parse([]byte(`
board: x
external_components:
  - source: github://vendor/board@v1.0.0
    components: [board]
`))
	if err != nil {
		t.Fatal(err)
	}
	s := p.External[0].Source
	if s.Type != "git" || s.URL != "https://github.com/vendor/board" || s.Ref != "v1.0.0" {
		t.Fatalf("shorthand parse failed: %#v", s)
	}
}
