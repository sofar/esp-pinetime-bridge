package handlers

import (
	"archive/zip"
	"bytes"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/sofar/pinetime-bridge-server/models"
	"github.com/sofar/pinetime-bridge-server/store"
)

type Handler struct {
	store          *store.Store
	firmwareDir    string
	lastHeartbeat  map[string]time.Time // bridge_id -> last heartbeat time
}

const bridgeOfflineThreshold = 90 * time.Second // 3 missed heartbeats (30s interval)

func New(s *store.Store, firmwareDir string) *Handler {
	os.MkdirAll(firmwareDir, 0755)
	return &Handler{
		store:         s,
		firmwareDir:   firmwareDir,
		lastHeartbeat: make(map[string]time.Time),
	}
}

func (h *Handler) serverLog(userID int64, level, msg string) {
	h.store.AddLog(&models.LogEntry{UserID: userID, Source: "server", Level: level, Message: msg})
}

func (h *Handler) Register(mux *http.ServeMux) {
	mux.HandleFunc("GET /api/users", h.ListUsers)
	mux.HandleFunc("POST /api/users", h.CreateUser)
	mux.HandleFunc("GET /api/users/{id}", h.GetUser)
	mux.HandleFunc("PUT /api/users/{id}", h.UpdateUser)
	mux.HandleFunc("DELETE /api/users/{id}", h.DeleteUser)

	mux.HandleFunc("GET /api/users/{id}/reminders", h.ListReminders)
	mux.HandleFunc("POST /api/users/{id}/reminders", h.CreateReminder)
	mux.HandleFunc("PUT /api/users/{id}/reminders/{rid}", h.UpdateReminder)
	mux.HandleFunc("DELETE /api/users/{id}/reminders/{rid}", h.DeleteReminder)

	mux.HandleFunc("POST /api/users/{id}/acks", h.CreateAck)
	mux.HandleFunc("GET /api/users/{id}/acks", h.ListAcks)

	mux.HandleFunc("POST /api/users/{id}/notifications", h.CreateNotification)
	mux.HandleFunc("GET /api/users/{id}/notifications/pending", h.ListPendingNotifications)
	mux.HandleFunc("PUT /api/notifications/{id}/delivered", h.MarkNotificationDelivered)
	mux.HandleFunc("POST /api/notifications/{id}/delivered", h.MarkNotificationDelivered)

	mux.HandleFunc("POST /api/bridges/{id}/status", h.UpdateBridgeStatus)
	mux.HandleFunc("GET /api/bridges/{id}/status", h.GetBridgeStatus)
	mux.HandleFunc("GET /api/bridges/{id}/battery-history", h.GetBatteryHistory)

	mux.HandleFunc("POST /api/users/{id}/logs", h.PostLog)
	mux.HandleFunc("GET /api/users/{id}/logs", h.ListLogs)

	mux.HandleFunc("GET /api/bridges/{id}/pairing", h.GetPairingState)
	mux.HandleFunc("POST /api/bridges/{id}/pairing", h.SetPairingState)
	mux.HandleFunc("POST /api/bridges/{id}/discovered", h.PostDiscoveredWatches)
	mux.HandleFunc("GET /api/bridges/{id}/discovered", h.GetDiscoveredWatches)

	mux.HandleFunc("POST /api/firmware/upload", h.UploadFirmware)
	mux.HandleFunc("GET /api/firmware/info", h.GetFirmwareInfo)
	mux.HandleFunc("GET /api/firmware/bin", h.GetFirmwareBin)
	mux.HandleFunc("GET /api/firmware/dat", h.GetFirmwareDat)
}

// JSON helpers

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(v)
}

func readJSON(r *http.Request, v any) error {
	r.Body = http.MaxBytesReader(nil, r.Body, 64<<10) // 64KB limit
	return json.NewDecoder(r.Body).Decode(v)
}

func pathID(r *http.Request, name string) (int64, error) {
	return strconv.ParseInt(r.PathValue(name), 10, 64)
}

// Users

func (h *Handler) ListUsers(w http.ResponseWriter, r *http.Request) {
	users, err := h.store.ListUsers()
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if users == nil {
		users = []models.User{}
	}
	writeJSON(w, http.StatusOK, users)
}

func (h *Handler) CreateUser(w http.ResponseWriter, r *http.Request) {
	var u models.User
	if err := readJSON(r, &u); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	if err := h.store.CreateUser(&u); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusCreated, u)
}

func (h *Handler) GetUser(w http.ResponseWriter, r *http.Request) {
	id, err := pathID(r, "id")
	if err != nil {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}
	u, err := h.store.GetUser(id)
	if errors.Is(err, sql.ErrNoRows) {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, u)
}

func (h *Handler) UpdateUser(w http.ResponseWriter, r *http.Request) {
	id, err := pathID(r, "id")
	if err != nil {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}
	var u models.User
	if err := readJSON(r, &u); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	u.ID = id
	if err := h.store.UpdateUser(&u); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, u)
}

func (h *Handler) DeleteUser(w http.ResponseWriter, r *http.Request) {
	id, err := pathID(r, "id")
	if err != nil {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}
	if err := h.store.DeleteUser(id); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// Reminders

func (h *Handler) ListReminders(w http.ResponseWriter, r *http.Request) {
	id, err := pathID(r, "id")
	if err != nil {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}
	reminders, err := h.store.ListReminders(id)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if reminders == nil {
		reminders = []models.Reminder{}
	}
	writeJSON(w, http.StatusOK, reminders)
}

func (h *Handler) CreateReminder(w http.ResponseWriter, r *http.Request) {
	userID, err := pathID(r, "id")
	if err != nil {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}
	var rem models.Reminder
	if err := readJSON(r, &rem); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	rem.UserID = userID

	// Auto-assign reminder_id if not explicitly set or if slot 0 is requested
	// Find the next available slot (0-19)
	existing, err := h.store.ListReminders(userID)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	used := make(map[uint8]bool)
	for _, e := range existing {
		used[e.ReminderID] = true
	}

	// If the caller didn't specify an ID, or specified one already in use, auto-assign
	if rem.ReminderID == 0 || used[rem.ReminderID] {
		assigned := false
		for id := uint8(0); id < 56; id++ {
			if !used[id] {
				rem.ReminderID = id
				assigned = true
				break
			}
		}
		if !assigned {
			http.Error(w, "no free reminder slots (max 56)", http.StatusConflict)
			return
		}
	}

	if err := h.store.CreateReminder(&rem); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	h.serverLog(userID, "info", fmt.Sprintf("Reminder #%d created: %02d:%02d \"%s\"", rem.ReminderID, rem.Hours, rem.Minutes, rem.Message))
	writeJSON(w, http.StatusCreated, rem)
}

func (h *Handler) UpdateReminder(w http.ResponseWriter, r *http.Request) {
	userID, err := pathID(r, "id")
	if err != nil {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}
	rid, err := strconv.Atoi(r.PathValue("rid"))
	if err != nil || rid < 0 || rid > 255 {
		http.Error(w, "invalid reminder id", http.StatusBadRequest)
		return
	}
	var rem models.Reminder
	if err := readJSON(r, &rem); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	rem.UserID = userID
	rem.ReminderID = uint8(rid)
	if err := h.store.UpdateReminder(&rem); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, rem)
}

func (h *Handler) DeleteReminder(w http.ResponseWriter, r *http.Request) {
	userID, err := pathID(r, "id")
	if err != nil {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}
	rid, err := strconv.Atoi(r.PathValue("rid"))
	if err != nil || rid < 0 || rid > 255 {
		http.Error(w, "invalid reminder id", http.StatusBadRequest)
		return
	}
	if err := h.store.DeleteReminder(userID, rid); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	h.serverLog(userID, "info", fmt.Sprintf("Reminder #%d deleted", rid))
	w.WriteHeader(http.StatusNoContent)
}

// Acks

func (h *Handler) CreateAck(w http.ResponseWriter, r *http.Request) {
	userID, err := pathID(r, "id")
	if err != nil {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}
	var ack models.ReminderAck
	if err := readJSON(r, &ack); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	ack.UserID = userID
	if err := h.store.CreateAck(&ack); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	h.serverLog(userID, "info", fmt.Sprintf("Reminder #%d acknowledged by watch", ack.ReminderID))
	writeJSON(w, http.StatusCreated, ack)
}

func (h *Handler) ListAcks(w http.ResponseWriter, r *http.Request) {
	userID, err := pathID(r, "id")
	if err != nil {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}
	acks, err := h.store.ListAcks(userID)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if acks == nil {
		acks = []models.ReminderAck{}
	}
	writeJSON(w, http.StatusOK, acks)
}

// Notifications

func (h *Handler) CreateNotification(w http.ResponseWriter, r *http.Request) {
	userID, err := pathID(r, "id")
	if err != nil {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}
	var n models.Notification
	if err := readJSON(r, &n); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	n.UserID = userID
	if err := h.store.CreateNotification(&n); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	h.serverLog(userID, "info", fmt.Sprintf("Notification queued: \"%s\"", n.Message))
	writeJSON(w, http.StatusCreated, n)
}

func (h *Handler) ListPendingNotifications(w http.ResponseWriter, r *http.Request) {
	userID, err := pathID(r, "id")
	if err != nil {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}
	notifs, err := h.store.ListPendingNotifications(userID)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if notifs == nil {
		notifs = []models.Notification{}
	}
	writeJSON(w, http.StatusOK, notifs)
}

func (h *Handler) MarkNotificationDelivered(w http.ResponseWriter, r *http.Request) {
	id, err := pathID(r, "id")
	if err != nil {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}
	if err := h.store.MarkNotificationDelivered(id); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// Bridge

func (h *Handler) UpdateBridgeStatus(w http.ResponseWriter, r *http.Request) {
	bridgeID := r.PathValue("id")
	var b models.BridgeStatus
	if err := readJSON(r, &b); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	b.BridgeID = bridgeID
	if err := h.store.UpdateBridgeStatus(&b); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}

	// Log bridge online/offline transitions (not every heartbeat)
	now := time.Now()
	last, seen := h.lastHeartbeat[bridgeID]
	if !seen || now.Sub(last) > bridgeOfflineThreshold {
		h.serverLog(0, "info", fmt.Sprintf("Bridge %s came online", bridgeID))
	}
	h.lastHeartbeat[bridgeID] = now

	w.WriteHeader(http.StatusNoContent)
}

func (h *Handler) GetBridgeStatus(w http.ResponseWriter, r *http.Request) {
	bridgeID := r.PathValue("id")
	b, err := h.store.GetBridgeStatus(bridgeID)
	if errors.Is(err, sql.ErrNoRows) {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	writeJSON(w, http.StatusOK, b)
}

func (h *Handler) GetBatteryHistory(w http.ResponseWriter, r *http.Request) {
	bridgeID := r.PathValue("id")
	days := 30
	if d := r.URL.Query().Get("days"); d != "" {
		if v, err := strconv.Atoi(d); err == nil && v > 0 && v <= 365 {
			days = v
		}
	}
	points, err := h.store.GetBatteryHistory(bridgeID, days)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if points == nil {
		points = []store.BatteryPoint{}
	}
	writeJSON(w, http.StatusOK, points)
}

// Firmware

func (h *Handler) UploadFirmware(w http.ResponseWriter, r *http.Request) {
	// Accept multipart form with a "file" field containing the DFU zip
	r.ParseMultipartForm(1 << 20) // 1MB max
	file, header, err := r.FormFile("file")
	if err != nil {
		http.Error(w, "file required: "+err.Error(), http.StatusBadRequest)
		return
	}
	defer file.Close()

	// Read the zip into memory
	var buf bytes.Buffer
	io.Copy(&buf, file)
	zipData := buf.Bytes()

	// Parse the zip to extract .bin and .dat files
	zr, err := zip.NewReader(bytes.NewReader(zipData), int64(len(zipData)))
	if err != nil {
		http.Error(w, "invalid zip: "+err.Error(), http.StatusBadRequest)
		return
	}

	var binData, datData []byte
	for _, f := range zr.File {
		rc, err := f.Open()
		if err != nil {
			continue
		}
		data, err := io.ReadAll(rc)
		rc.Close()
		if err != nil {
			http.Error(w, "failed to read zip entry: "+err.Error(), http.StatusBadRequest)
			return
		}

		if strings.HasSuffix(f.Name, ".bin") {
			binData = data
		} else if strings.HasSuffix(f.Name, ".dat") {
			datData = data
		}
	}

	if binData == nil || datData == nil {
		http.Error(w, "zip must contain .bin and .dat files", http.StatusBadRequest)
		return
	}

	// Write files to firmware directory
	if err := os.WriteFile(filepath.Join(h.firmwareDir, "firmware.bin"), binData, 0644); err != nil {
		http.Error(w, "failed to write firmware.bin: "+err.Error(), http.StatusInternalServerError)
		return
	}
	if err := os.WriteFile(filepath.Join(h.firmwareDir, "firmware.dat"), datData, 0644); err != nil {
		http.Error(w, "failed to write firmware.dat: "+err.Error(), http.StatusInternalServerError)
		return
	}

	// Write metadata
	version := strings.TrimSuffix(header.Filename, ".zip")
	version = strings.TrimPrefix(version, "pinetime-mcuboot-app-dfu-")

	// Extract git ref from binary (InfiniTime embeds "X.Y.Z-shortref" string)
	gitRef := ""
	re := regexp.MustCompile(`\d+\.\d+\.\d+-([0-9a-f]{8})`)
	if m := re.Find(binData); m != nil {
		parts := strings.SplitN(string(m), "-", 2)
		if len(parts) == 2 {
			gitRef = parts[1]
		}
	}

	info := models.FirmwareInfo{
		Version:    version,
		GitRef:     gitRef,
		Filename:   header.Filename,
		Size:       int64(len(zipData)),
		BinSize:    int64(len(binData)),
		DatSize:    int64(len(datData)),
		UploadedAt: time.Now().Format(time.RFC3339),
	}
	infoJSON, err := json.Marshal(info)
	if err != nil {
		http.Error(w, "failed to marshal info: "+err.Error(), http.StatusInternalServerError)
		return
	}
	if err := os.WriteFile(filepath.Join(h.firmwareDir, "info.json"), infoJSON, 0644); err != nil {
		http.Error(w, "failed to write info.json: "+err.Error(), http.StatusInternalServerError)
		return
	}

	h.serverLog(0, "info", fmt.Sprintf("Firmware uploaded: %s (%d bytes bin, %d bytes dat)", info.Version, info.BinSize, info.DatSize))
	writeJSON(w, http.StatusOK, info)
}

func (h *Handler) GetFirmwareInfo(w http.ResponseWriter, r *http.Request) {
	data, err := os.ReadFile(filepath.Join(h.firmwareDir, "info.json"))
	if err != nil {
		http.Error(w, "no firmware uploaded", http.StatusNotFound)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Write(data)
}

func (h *Handler) GetFirmwareBin(w http.ResponseWriter, r *http.Request) {
	path := filepath.Join(h.firmwareDir, "firmware.bin")
	data, err := os.ReadFile(path)
	if err != nil {
		http.Error(w, "no firmware", http.StatusNotFound)
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.Itoa(len(data)))
	w.Write(data)
}

func (h *Handler) GetFirmwareDat(w http.ResponseWriter, r *http.Request) {
	path := filepath.Join(h.firmwareDir, "firmware.dat")
	data, err := os.ReadFile(path)
	if err != nil {
		http.Error(w, "no firmware", http.StatusNotFound)
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.Itoa(len(data)))
	w.Write(data)
}

// Discovered watches

func (h *Handler) PostDiscoveredWatches(w http.ResponseWriter, r *http.Request) {
	bridgeID := r.PathValue("id")
	var watches []models.DiscoveredWatch
	if err := readJSON(r, &watches); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	h.store.SetDiscoveredWatches(bridgeID, watches)
	w.WriteHeader(http.StatusNoContent)
}

func (h *Handler) GetDiscoveredWatches(w http.ResponseWriter, r *http.Request) {
	bridgeID := r.PathValue("id")
	watches := h.store.GetDiscoveredWatches(bridgeID)
	writeJSON(w, http.StatusOK, watches)
}

// Pairing

func (h *Handler) GetPairingState(w http.ResponseWriter, r *http.Request) {
	bridgeID := r.PathValue("id")
	state := h.store.GetPairingState(bridgeID)
	writeJSON(w, http.StatusOK, state)
}

func (h *Handler) SetPairingState(w http.ResponseWriter, r *http.Request) {
	bridgeID := r.PathValue("id")
	var req models.PairingRequest
	if err := readJSON(r, &req); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	current := h.store.GetPairingState(bridgeID)

	switch req.State {
	case "dfu":
		// Web UI triggers DFU — bridge polls and initiates firmware push
		h.store.SetPairingState(bridgeID, &models.PairingRequest{State: "dfu"})
		h.store.AddLog(&models.LogEntry{UserID: 0, Source: "server", Level: "info", Message: fmt.Sprintf("DFU requested for bridge %s", bridgeID)})
	case "connecting":
		// Web UI selected a watch MAC — tell bridge to connect
		if req.Passkey == "" {
			http.Error(w, "mac required in passkey field", http.StatusBadRequest)
			return
		}
		h.store.SetPairingState(bridgeID, &models.PairingRequest{State: "connecting", Passkey: req.Passkey})
		h.store.AddLog(&models.LogEntry{UserID: 0, Source: "server", Level: "info", Message: fmt.Sprintf("Connecting to watch %s (bridge %s)", req.Passkey, bridgeID)})
	case "passkey_needed":
		// Bridge signals it needs a passkey from the user
		h.store.SetPairingState(bridgeID, &models.PairingRequest{State: "passkey_needed"})
		h.store.AddLog(&models.LogEntry{UserID: 0, Source: "bridge", Level: "info", Message: fmt.Sprintf("Watch pairing: enter passkey shown on watch (bridge %s)", bridgeID)})
	case "passkey_entered":
		// Web UI submits the passkey
		if req.Passkey == "" {
			http.Error(w, "passkey required", http.StatusBadRequest)
			return
		}
		current.Passkey = req.Passkey
		current.State = "passkey_entered"
		h.store.SetPairingState(bridgeID, current)
		h.store.AddLog(&models.LogEntry{UserID: 0, Source: "server", Level: "info", Message: fmt.Sprintf("Passkey entered for bridge %s", bridgeID)})
	case "paired":
		h.store.ClearPairingState(bridgeID)
		h.store.AddLog(&models.LogEntry{UserID: 0, Source: "bridge", Level: "info", Message: fmt.Sprintf("Watch paired successfully (bridge %s)", bridgeID)})
	case "failed":
		msg := "Pairing failed"
		if req.Passkey != "" {
			msg = req.Passkey // reuse field for error message
		}
		h.store.SetPairingState(bridgeID, &models.PairingRequest{State: "failed", Passkey: msg})
		h.store.AddLog(&models.LogEntry{UserID: 0, Source: "bridge", Level: "error", Message: fmt.Sprintf("Watch pairing failed: %s (bridge %s)", msg, bridgeID)})
	case "idle":
		h.store.ClearPairingState(bridgeID)
	default:
		http.Error(w, "invalid state", http.StatusBadRequest)
		return
	}

	writeJSON(w, http.StatusOK, h.store.GetPairingState(bridgeID))
}

// Logs

func (h *Handler) PostLog(w http.ResponseWriter, r *http.Request) {
	userID, err := pathID(r, "id")
	if err != nil {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}
	var entry models.LogEntry
	if err := readJSON(r, &entry); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	entry.UserID = userID
	if err := h.store.AddLog(&entry); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.WriteHeader(http.StatusCreated)
}

func (h *Handler) ListLogs(w http.ResponseWriter, r *http.Request) {
	userID, err := pathID(r, "id")
	if err != nil {
		http.Error(w, "invalid id", http.StatusBadRequest)
		return
	}
	logs, err := h.store.ListLogs(userID, 200)
	if err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	if logs == nil {
		logs = []models.LogEntry{}
	}
	writeJSON(w, http.StatusOK, logs)
}
