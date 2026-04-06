package store

import (
	"database/sql"
	"fmt"
	"log"
	"time"

	"github.com/sofar/pinetime-bridge-server/models"

	_ "modernc.org/sqlite"
)

const migrationSQL = `
CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    watch_mac TEXT NOT NULL,
    bridge_id TEXT NOT NULL,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS reminders (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL REFERENCES users(id),
    reminder_id INTEGER NOT NULL,
    hours INTEGER NOT NULL,
    minutes INTEGER NOT NULL,
    recurrence INTEGER NOT NULL DEFAULT 0,
    priority INTEGER NOT NULL DEFAULT 1,
    month INTEGER NOT NULL DEFAULT 0,
    day INTEGER NOT NULL DEFAULT 0,
    message TEXT NOT NULL DEFAULT '',
    enabled BOOLEAN NOT NULL DEFAULT 1,
    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    UNIQUE(user_id, reminder_id)
);
CREATE TABLE IF NOT EXISTS reminder_acks (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL REFERENCES users(id),
    reminder_id INTEGER NOT NULL,
    acked_at DATETIME NOT NULL,
    received_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS bridge_status (
    bridge_id TEXT PRIMARY KEY,
    connected BOOLEAN DEFAULT 0,
    watch_battery INTEGER DEFAULT 0,
    watch_firmware TEXT DEFAULT '',
    watch_manufacturer TEXT DEFAULT '',
    watch_software TEXT DEFAULT '',
    watch_steps INTEGER DEFAULT 0,
    last_sync TEXT DEFAULT '',
    bridge_ip TEXT DEFAULT '',
    last_heartbeat DATETIME DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS notifications (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL REFERENCES users(id),
    message TEXT NOT NULL,
    priority INTEGER NOT NULL DEFAULT 1,
    pending BOOLEAN NOT NULL DEFAULT 1,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
CREATE TABLE IF NOT EXISTS logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL DEFAULT 0,
    source TEXT NOT NULL DEFAULT 'server',
    level TEXT NOT NULL DEFAULT 'info',
    message TEXT NOT NULL,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_logs_user_created ON logs(user_id, created_at DESC);
CREATE TABLE IF NOT EXISTS battery_history (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    bridge_id TEXT NOT NULL,
    battery INTEGER NOT NULL,
    recorded_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX IF NOT EXISTS idx_battery_bridge_time ON battery_history(bridge_id, recorded_at DESC);
`

type Store struct {
	db         *sql.DB
	pairing    map[string]*models.PairingRequest    // bridge_id -> pairing state
	discovered map[string][]models.DiscoveredWatch   // bridge_id -> nearby watches
}

func Open(path string) *Store {
	db, err := sql.Open("sqlite", path)
	if err != nil {
		log.Fatalf("Failed to open database: %v", err)
	}
	db.SetMaxOpenConns(1) // SQLite doesn't handle concurrent writes well
	return &Store{
		db:         db,
		pairing:    make(map[string]*models.PairingRequest),
		discovered: make(map[string][]models.DiscoveredWatch),
	}
}

func (s *Store) Close() {
	s.db.Close()
}

func (s *Store) Migrate() {
	if _, err := s.db.Exec(migrationSQL); err != nil {
		log.Fatalf("Failed to run migrations: %v", err)
	}
}

// Users

func (s *Store) CreateUser(u *models.User) error {
	res, err := s.db.Exec(
		"INSERT INTO users (name, watch_mac, bridge_id) VALUES (?, ?, ?)",
		u.Name, u.WatchMAC, u.BridgeID,
	)
	if err != nil {
		return err
	}
	u.ID, _ = res.LastInsertId()
	return nil
}

func (s *Store) GetUser(id int64) (*models.User, error) {
	u := &models.User{}
	err := s.db.QueryRow("SELECT id, name, watch_mac, bridge_id, created_at FROM users WHERE id = ?", id).
		Scan(&u.ID, &u.Name, &u.WatchMAC, &u.BridgeID, &u.CreatedAt)
	if err != nil {
		return nil, err
	}
	return u, nil
}

func (s *Store) ListUsers() ([]models.User, error) {
	rows, err := s.db.Query("SELECT id, name, watch_mac, bridge_id, created_at FROM users ORDER BY id")
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var users []models.User
	for rows.Next() {
		var u models.User
		if err := rows.Scan(&u.ID, &u.Name, &u.WatchMAC, &u.BridgeID, &u.CreatedAt); err != nil {
			return nil, err
		}
		users = append(users, u)
	}
	return users, nil
}

func (s *Store) UpdateUser(u *models.User) error {
	_, err := s.db.Exec("UPDATE users SET name = ?, watch_mac = ?, bridge_id = ? WHERE id = ?",
		u.Name, u.WatchMAC, u.BridgeID, u.ID)
	return err
}

func (s *Store) DeleteUser(id int64) error {
	_, err := s.db.Exec("DELETE FROM users WHERE id = ?", id)
	return err
}

// Reminders

func (s *Store) CreateReminder(r *models.Reminder) error {
	res, err := s.db.Exec(
		`INSERT INTO reminders (user_id, reminder_id, hours, minutes, recurrence, priority, month, day, message, enabled)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		 ON CONFLICT(user_id, reminder_id) DO UPDATE SET
		   hours=excluded.hours, minutes=excluded.minutes, recurrence=excluded.recurrence,
		   priority=excluded.priority, month=excluded.month, day=excluded.day,
		   message=excluded.message, enabled=excluded.enabled,
		   updated_at=CURRENT_TIMESTAMP`,
		r.UserID, r.ReminderID, r.Hours, r.Minutes, r.Recurrence, r.Priority, r.Month, r.Day, r.Message, r.Enabled,
	)
	if err != nil {
		return err
	}
	r.ID, _ = res.LastInsertId()
	return nil
}

func (s *Store) ListReminders(userID int64) ([]models.Reminder, error) {
	rows, err := s.db.Query(
		"SELECT id, user_id, reminder_id, hours, minutes, recurrence, priority, month, day, message, enabled, updated_at FROM reminders WHERE user_id = ? ORDER BY reminder_id",
		userID,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var reminders []models.Reminder
	for rows.Next() {
		var r models.Reminder
		if err := rows.Scan(&r.ID, &r.UserID, &r.ReminderID, &r.Hours, &r.Minutes, &r.Recurrence, &r.Priority, &r.Month, &r.Day, &r.Message, &r.Enabled, &r.UpdatedAt); err != nil {
			return nil, err
		}
		reminders = append(reminders, r)
	}
	return reminders, nil
}

func (s *Store) UpdateReminder(r *models.Reminder) error {
	_, err := s.db.Exec(
		"UPDATE reminders SET hours=?, minutes=?, recurrence=?, priority=?, month=?, day=?, message=?, enabled=?, updated_at=? WHERE user_id=? AND reminder_id=?",
		r.Hours, r.Minutes, r.Recurrence, r.Priority, r.Month, r.Day, r.Message, r.Enabled, time.Now(), r.UserID, r.ReminderID,
	)
	return err
}

func (s *Store) DeleteReminder(userID int64, reminderID int) error {
	_, err := s.db.Exec("DELETE FROM reminders WHERE user_id = ? AND reminder_id = ?", userID, reminderID)
	return err
}

// Acks

func (s *Store) CreateAck(a *models.ReminderAck) error {
	res, err := s.db.Exec(
		"INSERT INTO reminder_acks (user_id, reminder_id, acked_at) VALUES (?, ?, ?)",
		a.UserID, a.ReminderID, a.AckedAt,
	)
	if err != nil {
		return err
	}
	a.ID, _ = res.LastInsertId()
	return nil
}

func (s *Store) ListAcks(userID int64) ([]models.ReminderAck, error) {
	rows, err := s.db.Query(
		"SELECT id, user_id, reminder_id, acked_at, received_at FROM reminder_acks WHERE user_id = ? ORDER BY received_at DESC LIMIT 100",
		userID,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var acks []models.ReminderAck
	for rows.Next() {
		var a models.ReminderAck
		if err := rows.Scan(&a.ID, &a.UserID, &a.ReminderID, &a.AckedAt, &a.ReceivedAt); err != nil {
			return nil, err
		}
		acks = append(acks, a)
	}
	return acks, nil
}

// Bridge Status

func (s *Store) UpdateBridgeStatus(b *models.BridgeStatus) error {
	_, err := s.db.Exec(
		`INSERT INTO bridge_status (bridge_id, connected, watch_battery, watch_firmware, watch_manufacturer, watch_software, watch_steps, last_sync, bridge_ip, last_heartbeat)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP)
		 ON CONFLICT(bridge_id) DO UPDATE SET
		   connected=excluded.connected, watch_battery=excluded.watch_battery,
		   watch_firmware=CASE WHEN excluded.watch_firmware != '' THEN excluded.watch_firmware ELSE bridge_status.watch_firmware END,
		   watch_manufacturer=CASE WHEN excluded.watch_manufacturer != '' THEN excluded.watch_manufacturer ELSE bridge_status.watch_manufacturer END,
		   watch_software=CASE WHEN excluded.watch_software != '' THEN excluded.watch_software ELSE bridge_status.watch_software END,
		   watch_steps=excluded.watch_steps,
		   last_sync=CASE WHEN excluded.last_sync != '' THEN excluded.last_sync ELSE bridge_status.last_sync END,
		   bridge_ip=CASE WHEN excluded.bridge_ip != '' THEN excluded.bridge_ip ELSE bridge_status.bridge_ip END,
		   last_heartbeat=CURRENT_TIMESTAMP`,
		b.BridgeID, b.Connected, b.WatchBattery, b.WatchFirmware, b.WatchManufacturer, b.WatchSoftware, b.WatchSteps, b.LastSync, b.BridgeIP,
	)
	if err != nil {
		return err
	}
	// Record battery history (only if battery > 0, i.e. watch is reporting)
	if b.WatchBattery > 0 {
		s.db.Exec(`INSERT INTO battery_history (bridge_id, battery) VALUES (?, ?)`, b.BridgeID, b.WatchBattery)
	}
	return nil
}

type BatteryPoint struct {
	RecordedAt time.Time `json:"recorded_at"`
	Battery    uint8     `json:"battery"`
}

func (s *Store) GetBatteryHistory(bridgeID string, days int) ([]BatteryPoint, error) {
	rows, err := s.db.Query(
		`SELECT battery, recorded_at FROM battery_history
		 WHERE bridge_id = ? AND recorded_at > datetime('now', ?)
		 ORDER BY recorded_at ASC`,
		bridgeID, fmt.Sprintf("-%d days", days),
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var points []BatteryPoint
	for rows.Next() {
		var p BatteryPoint
		if err := rows.Scan(&p.Battery, &p.RecordedAt); err != nil {
			continue
		}
		points = append(points, p)
	}
	return points, nil
}

func (s *Store) GetBridgeStatus(bridgeID string) (*models.BridgeStatus, error) {
	b := &models.BridgeStatus{}
	err := s.db.QueryRow("SELECT bridge_id, connected, watch_battery, COALESCE(watch_firmware,''), COALESCE(watch_manufacturer,''), COALESCE(watch_software,''), COALESCE(watch_steps,0), COALESCE(last_sync,''), COALESCE(bridge_ip,''), last_heartbeat FROM bridge_status WHERE bridge_id = ?", bridgeID).
		Scan(&b.BridgeID, &b.Connected, &b.WatchBattery, &b.WatchFirmware, &b.WatchManufacturer, &b.WatchSoftware, &b.WatchSteps, &b.LastSync, &b.BridgeIP, &b.LastHeartbeat)
	if err != nil {
		return nil, err
	}
	return b, nil
}

// Notifications

func (s *Store) CreateNotification(n *models.Notification) error {
	res, err := s.db.Exec(
		"INSERT INTO notifications (user_id, message, priority) VALUES (?, ?, ?)",
		n.UserID, n.Message, n.Priority,
	)
	if err != nil {
		return err
	}
	n.ID, _ = res.LastInsertId()
	return nil
}

func (s *Store) ListPendingNotifications(userID int64) ([]models.Notification, error) {
	rows, err := s.db.Query(
		"SELECT id, user_id, message, priority, pending, created_at FROM notifications WHERE user_id = ? AND pending = 1 ORDER BY created_at",
		userID,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var notifs []models.Notification
	for rows.Next() {
		var n models.Notification
		if err := rows.Scan(&n.ID, &n.UserID, &n.Message, &n.Priority, &n.Pending, &n.CreatedAt); err != nil {
			return nil, err
		}
		notifs = append(notifs, n)
	}
	return notifs, nil
}

func (s *Store) MarkNotificationDelivered(id int64) error {
	_, err := s.db.Exec("UPDATE notifications SET pending = 0 WHERE id = ?", id)
	return err
}

// Logs

func (s *Store) AddLog(entry *models.LogEntry) error {
	_, err := s.db.Exec(
		"INSERT INTO logs (user_id, source, level, message) VALUES (?, ?, ?, ?)",
		entry.UserID, entry.Source, entry.Level, entry.Message,
	)
	return err
}

func (s *Store) ListLogs(userID int64, limit int) ([]models.LogEntry, error) {
	rows, err := s.db.Query(
		"SELECT id, user_id, source, level, message, created_at FROM logs WHERE user_id = ? OR user_id = 0 ORDER BY created_at DESC LIMIT ?",
		userID, limit,
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var logs []models.LogEntry
	for rows.Next() {
		var e models.LogEntry
		if err := rows.Scan(&e.ID, &e.UserID, &e.Source, &e.Level, &e.Message, &e.CreatedAt); err != nil {
			return nil, err
		}
		logs = append(logs, e)
	}
	return logs, nil
}

// Pairing (in-memory, transient)

// Discovered watches (in-memory, transient)

func (s *Store) SetDiscoveredWatches(bridgeID string, watches []models.DiscoveredWatch) {
	s.discovered[bridgeID] = watches
}

func (s *Store) GetDiscoveredWatches(bridgeID string) []models.DiscoveredWatch {
	if w, ok := s.discovered[bridgeID]; ok {
		return w
	}
	return []models.DiscoveredWatch{}
}

func (s *Store) SetPairingState(bridgeID string, req *models.PairingRequest) {
	req.BridgeID = bridgeID
	s.pairing[bridgeID] = req
}

func (s *Store) GetPairingState(bridgeID string) *models.PairingRequest {
	if p, ok := s.pairing[bridgeID]; ok {
		return p
	}
	return &models.PairingRequest{BridgeID: bridgeID, State: "idle"}
}

func (s *Store) ClearPairingState(bridgeID string) {
	delete(s.pairing, bridgeID)
}

func (s *Store) PruneLogs(keepCount int) error {
	_, err := s.db.Exec(
		"DELETE FROM logs WHERE id NOT IN (SELECT id FROM logs ORDER BY created_at DESC LIMIT ?)",
		keepCount,
	)
	return err
}
