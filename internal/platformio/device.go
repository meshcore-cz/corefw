package platformio

import (
	"context"
	"encoding/json"
	"fmt"
	"os/exec"
	"strings"
)

type DeviceListOptions struct {
	All      bool
	LookPath func(string) (string, error)
}

type Device struct {
	Port        string `json:"port"`
	Description string `json:"description,omitempty"`
	HardwareID  string `json:"hardware_id,omitempty"`
	BoardHint   string `json:"board_hint,omitempty"`
}

func ListDevices(ctx context.Context, opts DeviceListOptions) ([]Device, error) {
	lookPath := opts.LookPath
	if lookPath == nil {
		lookPath = exec.LookPath
	}
	if _, err := lookPath("pio"); err != nil {
		return nil, ErrNotFound
	}
	cmd := exec.CommandContext(ctx, "pio", "device", "list", "--json-output")
	out, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("PlatformIO device list failed: %w", err)
	}
	return ParseDevices(out)
}

func ParseDevices(data []byte) ([]Device, error) {
	var raw []map[string]any
	if err := json.Unmarshal(data, &raw); err != nil {
		return nil, fmt.Errorf("parse PlatformIO device JSON: %w", err)
	}
	devices := make([]Device, 0, len(raw))
	for _, item := range raw {
		dev := Device{
			Port:        firstString(item, "port", "name", "path"),
			Description: firstString(item, "description", "desc"),
			HardwareID:  firstString(item, "hwid", "hardware_id", "hardware"),
		}
		dev.BoardHint = boardHint(dev)
		if dev.Port != "" {
			devices = append(devices, dev)
		}
	}
	return devices, nil
}

func firstString(m map[string]any, keys ...string) string {
	for _, key := range keys {
		if v, ok := m[key].(string); ok {
			return v
		}
	}
	return ""
}

func boardHint(dev Device) string {
	text := strings.ToLower(dev.Description + " " + dev.HardwareID)
	switch {
	case strings.Contains(text, "wio tracker"):
		return "wio-tracker-l1"
	case strings.Contains(text, "heltec"):
		return "heltec-v3"
	default:
		return ""
	}
}
