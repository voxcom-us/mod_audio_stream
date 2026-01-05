DROP TABLE IF EXISTS version;
CREATE TABLE version (
    table_name VARCHAR(128) DEFAULT '' NOT NULL,
    table_version INTEGER DEFAULT 0 NOT NULL
);

INSERT INTO "version" values ('address', '6');
INSERT INTO "version" values ('trusted','6');

DROP TABLE IF EXISTS trusted;
CREATE TABLE trusted (
    id INTEGER PRIMARY KEY NOT NULL,
    src_ip VARCHAR(50) NOT NULL,
    proto VARCHAR(4) NOT NULL,
    from_pattern VARCHAR(64) DEFAULT NULL,
    ruri_pattern VARCHAR(64) DEFAULT NULL,
    tag VARCHAR(64),
    priority INTEGER DEFAULT 0 NOT NULL
);

DROP INDEX IF EXISTS trusted_peer_idx;
CREATE INDEX trusted_peer_idx ON trusted (src_ip);

DROP TABLE IF EXISTS address;
CREATE TABLE address (
    id INTEGER PRIMARY KEY NOT NULL,
    grp INTEGER DEFAULT 1 NOT NULL,
    ip_addr VARCHAR(50) NOT NULL,
    mask INTEGER DEFAULT 32 NOT NULL,
    port SMALLINT DEFAULT 0 NOT NULL,
    tag VARCHAR(64)
);

INSERT INTO "address" (grp, ip_addr, mask, tag) VALUES ('1', '10.2.0.0', '16', 'private');
