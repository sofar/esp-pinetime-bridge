package models

import "time"

type User struct {
	ID        int64     `json:"id"`
	Name      string    `json:"name"`
	WatchMAC  string    `json:"watch_mac"`
	BridgeID  string    `json:"bridge_id"`
	CreatedAt time.Time `json:"created_at"`
}

type Reminder struct {
	ID         int64     `json:"id"`
	UserID     int64     `json:"user_id"`
	ReminderID uint8     `json:"reminder_id"`
	Hours      uint8     `json:"hours"`
	Minutes    uint8     `json:"minutes"`
	Recurrence uint8     `json:"recurrence"`
	Priority   uint8     `json:"priority"`
	Month      uint8     `json:"month"`
	Day        uint8     `json:"day"`
	Message    string    `json:"message"`
	Enabled    bool      `json:"enabled"`
	UpdatedAt  time.Time `json:"updated_at"`
}

type ReminderAck struct {
	ID         int64     `json:"id"`
	UserID     int64     `json:"user_id"`
	ReminderID uint8     `json:"reminder_id"`
	AckedAt    time.Time `json:"acked_at"`
	ReceivedAt time.Time `json:"received_at"`
}

type DiscoveredWatch struct {
	MAC  string `json:"mac"`
	Name string `json:"name"`
	RSSI int    `json:"rssi"`
}

type PairingRequest struct {
	BridgeID string `json:"bridge_id"`
	Passkey  string `json:"passkey"`
	State    string `json:"state"` // "waiting", "passkey_needed", "passkey_entered", "paired", "failed"
}

type BridgeStatus struct {
	BridgeID          string    `json:"bridge_id"`
	Connected         bool      `json:"connected"`
	WatchBattery      uint8     `json:"watch_battery"`
	WatchFirmware     string    `json:"watch_firmware"`
	WatchManufacturer string    `json:"watch_manufacturer"`
	WatchSoftware     string    `json:"watch_software"`
	WatchSteps        uint32    `json:"watch_steps"`
	WatchUptime       uint32    `json:"watch_uptime"`
	LastSync          string    `json:"last_sync"`
	BridgeIP          string    `json:"bridge_ip"`
	LastHeartbeat     time.Time `json:"last_heartbeat"`
}

type Notification struct {
	ID        int64     `json:"id"`
	UserID    int64     `json:"user_id"`
	Message   string    `json:"message"`
	Priority  uint8     `json:"priority"`
	Pending   bool      `json:"pending"`
	CreatedAt time.Time `json:"created_at"`
}

type FirmwareInfo struct {
	Version    string `json:"version"`
	GitRef     string `json:"git_ref"`
	Filename   string `json:"filename"`
	Size       int64  `json:"size"`
	BinSize    int64  `json:"bin_size"`
	DatSize    int64  `json:"dat_size"`
	UploadedAt string `json:"uploaded_at"`
}

type LogEntry struct {
	ID        int64     `json:"id"`
	UserID    int64     `json:"user_id"`
	Source    string    `json:"source"`  // "server", "bridge", "watch"
	Level     string    `json:"level"`   // "info", "warn", "error"
	Message   string    `json:"message"`
	CreatedAt time.Time `json:"created_at"`
}
