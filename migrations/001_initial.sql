CREATE TABLE IF NOT EXISTS admin_users (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  username VARCHAR(64) NOT NULL,
  display_name VARCHAR(64) NOT NULL,
  password_hash VARCHAR(100) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  enabled BOOLEAN NOT NULL DEFAULT TRUE,
  created_at DATETIME(3) NOT NULL,
  updated_at DATETIME(3) NOT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uk_admin_users_username (username),
  CONSTRAINT chk_admin_users_display_name CHECK (CHAR_LENGTH(display_name) BETWEEN 1 AND 64)
) ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_as_cs;

CREATE TABLE IF NOT EXISTS devices (
  device_id VARCHAR(64) NOT NULL,
  device_name VARCHAR(100) NOT NULL,
  http_token_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  mqtt_username VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  enabled BOOLEAN NOT NULL DEFAULT TRUE,
  created_at DATETIME(3) NOT NULL,
  updated_at DATETIME(3) NOT NULL,
  PRIMARY KEY (device_id),
  UNIQUE KEY uk_devices_http_token_hash (http_token_hash),
  UNIQUE KEY uk_devices_mqtt_username (mqtt_username)
) ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_as_cs;

CREATE TABLE IF NOT EXISTS recognition_logs (
  recognition_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  device_id VARCHAR(64) NOT NULL,
  capture_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  image_sha256 CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  revision BIGINT UNSIGNED NOT NULL,
  status ENUM('PROCESSING','SUCCEEDED','FAILED') NOT NULL,
  plate_number VARCHAR(16) NULL,
  error_code VARCHAR(64) NULL,
  error_message VARCHAR(512) NULL,
  image_path VARCHAR(512) NOT NULL,
  image_mime ENUM('image/jpeg','image/png') NOT NULL,
  image_size_bytes BIGINT UNSIGNED NOT NULL,
  captured_at DATETIME(3) NOT NULL,
  started_at DATETIME(3) NOT NULL,
  completed_at DATETIME(3) NULL,
  duration_ms BIGINT UNSIGNED NULL,
  created_at DATETIME(3) NOT NULL,
  updated_at DATETIME(3) NOT NULL,
  PRIMARY KEY (recognition_id),
  UNIQUE KEY uk_recognition_device_capture (device_id, capture_id),
  KEY idx_recognition_captured (captured_at DESC, recognition_id DESC),
  KEY idx_recognition_device_captured (device_id, captured_at DESC, recognition_id DESC),
  CONSTRAINT fk_recognition_device FOREIGN KEY (device_id) REFERENCES devices(device_id),
  CONSTRAINT chk_recognition_revision CHECK (revision >= 1 AND revision <= 9007199254740991),
  CONSTRAINT chk_recognition_image_size CHECK (image_size_bytes <= 10485760),
  CONSTRAINT chk_recognition_state CHECK (
    (status = 'PROCESSING' AND plate_number IS NULL AND error_code IS NULL
      AND error_message IS NULL AND completed_at IS NULL AND duration_ms IS NULL)
    OR
    (status = 'SUCCEEDED' AND plate_number IS NOT NULL AND error_code IS NULL
      AND error_message IS NULL AND completed_at IS NOT NULL AND duration_ms IS NOT NULL)
    OR
    (status = 'FAILED' AND plate_number IS NULL AND error_code IS NOT NULL
      AND error_message IS NOT NULL AND completed_at IS NOT NULL AND duration_ms IS NOT NULL)
  )
) ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_as_cs;

CREATE TABLE IF NOT EXISTS access_lists (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  list_type ENUM('WHITE','BLACK') NOT NULL,
  plate_number VARCHAR(16) NOT NULL,
  remark VARCHAR(200) NOT NULL DEFAULT '',
  created_by_user_id BIGINT UNSIGNED NOT NULL,
  created_by_display_name VARCHAR(64) NOT NULL,
  created_at DATETIME(3) NOT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uk_access_lists_plate (plate_number),
  KEY idx_access_lists_type_created (list_type, created_at DESC, id DESC),
  CONSTRAINT fk_access_lists_user FOREIGN KEY (created_by_user_id) REFERENCES admin_users(id),
  CONSTRAINT chk_access_lists_plate CHECK (CHAR_LENGTH(plate_number) BETWEEN 1 AND 16),
  CONSTRAINT chk_access_lists_remark CHECK (CHAR_LENGTH(remark) <= 200)
) ENGINE=InnoDB DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_as_cs;

INSERT INTO admin_users (
  id, username, display_name, password_hash, enabled, created_at, updated_at)
SELECT
  1, 'admin', '演示管理员',
  '$2b$12$abcdefghijklmnopqrstuumj.RgFt55etWFiErc2FEn.hnLKiyjYi',
  TRUE, UTC_TIMESTAMP(3), UTC_TIMESTAMP(3)
WHERE NOT EXISTS (
  SELECT 1 FROM admin_users WHERE id = 1
);

INSERT INTO devices (
  device_id, device_name, http_token_hash, mqtt_username, enabled, created_at, updated_at)
SELECT
  'device-001', '入口设备',
  '3ce1a47030c0f5fdcd04315f246c4ef4c5f70bc70e8320efdac6c092c8fe1186',
  'device-001', TRUE, UTC_TIMESTAMP(3), UTC_TIMESTAMP(3)
WHERE NOT EXISTS (
  SELECT 1 FROM devices WHERE device_id = 'device-001'
);
