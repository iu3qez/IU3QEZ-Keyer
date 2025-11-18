-- CWNet Protocol Dissector for Wireshark
-- Remote CW Keyer Protocol (DL4YHF)
-- Place this file in: ~/.local/lib/wireshark/plugins/ (Linux) or %APPDATA%\Wireshark\plugins\ (Windows)
-- Or load it via: wireshark -X lua_script:cwnet_dissector.lua

cwnet_protocol = Proto("CWNet", "Remote CW Keyer Protocol")

-- Protocol fields
local f_command = ProtoField.uint8("cwnet.command", "Command", base.HEX)
local f_command_type = ProtoField.uint8("cwnet.command.type", "Command Type", base.HEX, nil, 0x3F)
local f_length_type = ProtoField.uint8("cwnet.command.lengthtype", "Length Type", base.HEX, {
    [0x00] = "No Payload",
    [0x40] = "Short Block (1 byte)",
    [0x80] = "Long Block (2 bytes)",
    [0xC0] = "Reserved"
}, 0xC0)
local f_length = ProtoField.uint16("cwnet.length", "Payload Length", base.DEC)
local f_payload = ProtoField.bytes("cwnet.payload", "Payload")

-- CONNECT fields
local f_connect_username = ProtoField.string("cwnet.connect.username", "Username")
local f_connect_callsign = ProtoField.string("cwnet.connect.callsign", "Callsign")
local f_connect_permissions = ProtoField.uint32("cwnet.connect.permissions", "Permissions", base.HEX)

-- PING fields
local f_ping_type = ProtoField.uint8("cwnet.ping.type", "Ping Type", base.DEC, {
    [0] = "REQUEST",
    [1] = "RESPONSE_1",
    [2] = "RESPONSE_2"
})
local f_ping_id = ProtoField.uint8("cwnet.ping.id", "Sequence ID", base.DEC)
local f_ping_t0 = ProtoField.uint32("cwnet.ping.t0", "Timestamp T0 (ms)", base.DEC)
local f_ping_t1 = ProtoField.uint32("cwnet.ping.t1", "Timestamp T1 (ms)", base.DEC)
local f_ping_t2 = ProtoField.uint32("cwnet.ping.t2", "Timestamp T2 (ms)", base.DEC)

-- MORSE fields
local f_morse_byte = ProtoField.uint8("cwnet.morse.byte", "CW Byte", base.HEX)
local f_morse_key = ProtoField.uint8("cwnet.morse.key", "Key State", base.DEC, {
    [0] = "UP",
    [1] = "DOWN"
}, 0x80)
local f_morse_timestamp_raw = ProtoField.uint8("cwnet.morse.timestamp_raw", "Timestamp (raw)", base.HEX, nil, 0x7F)
local f_morse_timestamp_ms = ProtoField.uint32("cwnet.morse.timestamp_ms", "Timestamp (ms)", base.DEC)

-- PRINT fields
local f_print_message = ProtoField.string("cwnet.print.message", "Message")

-- RIGCTLD fields
local f_rigctld_command = ProtoField.string("cwnet.rigctld.command", "Hamlib Command")

cwnet_protocol.fields = {
    f_command, f_command_type, f_length_type, f_length, f_payload,
    f_connect_username, f_connect_callsign, f_connect_permissions,
    f_ping_type, f_ping_id, f_ping_t0, f_ping_t1, f_ping_t2,
    f_morse_byte, f_morse_key, f_morse_timestamp_raw, f_morse_timestamp_ms,
    f_print_message, f_rigctld_command
}

-- Command names
local command_names = {
    [0x01] = "CONNECT",
    [0x02] = "DISCONNECT",
    [0x03] = "PING",
    [0x04] = "PRINT",
    [0x05] = "TX_INFO",
    [0x06] = "RIGCTLD",
    [0x10] = "MORSE",
    [0x11] = "AUDIO",
    [0x12] = "VORBIS",
    [0x14] = "CI_V",
    [0x15] = "SPECTRUM",
    [0x16] = "FREQ_REPORT",
    [0x18] = "PARAM_INTEGER",
    [0x19] = "PARAM_DOUBLE",
    [0x1A] = "PARAM_STRING",
    [0x20] = "METER_REPORT",
    [0x21] = "POTI_REPORT",
    [0x31] = "TUNNEL_1",
    [0x32] = "TUNNEL_2",
    [0x33] = "TUNNEL_3"
}

-- Decode 7-bit non-linear timestamp to milliseconds (per protocol spec)
local function decode_timestamp(encoded)
    local ts = bit.band(encoded, 0x7F)
    if ts <= 0x1F then
        return ts
    elseif ts <= 0x3F then
        return 32 + 4 * (ts - 0x20)
    else
        return 157 + 16 * (ts - 0x40)
    end
end

-- Parse CONNECT payload
local function parse_connect(buffer, tree, offset, length)
    if length < 92 then
        tree:add_expert_info(PI_MALFORMED, PI_ERROR, "CONNECT payload too short")
        return
    end

    -- Username (44 bytes, null-terminated)
    local username = buffer(offset, 44):stringz()
    tree:add(f_connect_username, buffer(offset, 44), username)
    offset = offset + 44

    -- Callsign (44 bytes, null-terminated)
    local callsign = buffer(offset, 44):stringz()
    tree:add(f_connect_callsign, buffer(offset, 44), callsign)
    offset = offset + 44

    -- Permissions (4 bytes, little-endian)
    local permissions = buffer(offset, 4):le_uint()
    local perm_tree = tree:add(f_connect_permissions, buffer(offset, 4), permissions)

    -- Decode permission bits
    local perm_bits = {}
    if bit.band(permissions, 0x01) ~= 0 then table.insert(perm_bits, "TALK") end
    if bit.band(permissions, 0x02) ~= 0 then table.insert(perm_bits, "TRANSMIT") end
    if bit.band(permissions, 0x04) ~= 0 then table.insert(perm_bits, "CTRL_RIG") end
    if bit.band(permissions, 0x08) ~= 0 then table.insert(perm_bits, "ADMIN") end

    if #perm_bits > 0 then
        perm_tree:append_text(" (" .. table.concat(perm_bits, ", ") .. ")")
    else
        perm_tree:append_text(" (NONE)")
    end
end

-- Parse PING payload
local function parse_ping(buffer, tree, offset, length)
    if length < 16 then
        tree:add_expert_info(PI_MALFORMED, PI_ERROR, "PING payload too short")
        return
    end

    local ping_type = buffer(offset, 1):uint()
    tree:add(f_ping_type, buffer(offset, 1))
    offset = offset + 1

    tree:add(f_ping_id, buffer(offset, 1))
    offset = offset + 1

    -- Skip 2 reserved bytes
    offset = offset + 2

    -- Timestamps (little-endian uint32)
    local t0 = buffer(offset, 4):le_uint()
    tree:add(f_ping_t0, buffer(offset, 4), t0)
    offset = offset + 4

    if length >= 12 then
        local t1 = buffer(offset, 4):le_uint()
        tree:add(f_ping_t1, buffer(offset, 4), t1)
        offset = offset + 4
    end

    if length >= 16 then
        local t2 = buffer(offset, 4):le_uint()
        tree:add(f_ping_t2, buffer(offset, 4), t2)

        -- Calculate RTT if we have all timestamps
        if ping_type == 2 and t0 > 0 and t2 > t0 then
            local rtt = t2 - t0
            local latency = rtt / 2
            tree:append_text(string.format(" [RTT: %d ms, Latency: %d ms]", rtt, latency))
        end
    end
end

-- Parse MORSE payload (KEY UP/DOWN with timestamps)
local function parse_morse(buffer, tree, offset, length)
    if length == 0 then
        return
    end

    local morse_tree = tree:add(cwnet_protocol, buffer(offset, length), "CW Stream (" .. length .. " events)")

    for i = 0, length - 1 do
        local cw_byte = buffer(offset + i, 1):uint()
        local key_down = bit.band(cw_byte, 0x80) ~= 0
        local ts_raw = bit.band(cw_byte, 0x7F)
        local ts_ms = decode_timestamp(ts_raw)

        local event_tree = morse_tree:add(f_morse_byte, buffer(offset + i, 1), cw_byte)
        event_tree:add(f_morse_key, buffer(offset + i, 1), key_down and 1 or 0)
        event_tree:add(f_morse_timestamp_raw, buffer(offset + i, 1), ts_raw)
        event_tree:add(f_morse_timestamp_ms, ts_ms)

        -- Summary text
        local key_state = key_down and "DOWN" or "UP"
        event_tree:set_text(string.format("CW Event %d: Key %s after %d ms (0x%02X)", i + 1, key_state, ts_ms, cw_byte))
    end

    -- Check for End-Of-Transmission (two consecutive key-up bytes)
    if length >= 2 then
        local last = buffer(offset + length - 1, 1):uint()
        local prev = buffer(offset + length - 2, 1):uint()
        if bit.band(last, 0x80) == 0 and bit.band(prev, 0x80) == 0 then
            morse_tree:append_text(" [End-Of-Transmission detected]")
        end
    end
end

-- Parse PRINT payload
local function parse_print(buffer, tree, offset, length)
    if length > 0 then
        local message = buffer(offset, length):string()
        tree:add(f_print_message, buffer(offset, length), message)
    end
end

-- Parse RIGCTLD payload
local function parse_rigctld(buffer, tree, offset, length)
    if length > 0 then
        local command = buffer(offset, length):stringz()
        tree:add(f_rigctld_command, buffer(offset, length), command)
    end
end

-- Main dissector function
function cwnet_protocol.dissector(buffer, pinfo, tree)
    local length = buffer:len()
    if length == 0 then return end

    pinfo.cols.protocol = "CWNet"

    local offset = 0
    local frame_count = 0
    local info_text = {}  -- Collect command names in a table

    while offset < length do
        frame_count = frame_count + 1

        -- Parse command byte
        local command_byte = buffer(offset, 1):uint()
        local cmd_type = bit.band(command_byte, 0x3F)
        local len_type = bit.band(command_byte, 0xC0)

        -- Determine payload length
        local header_size = 1
        local payload_length = 0

        if len_type == 0x00 then
            -- No payload
            header_size = 1
            payload_length = 0
        elseif len_type == 0x40 then
            -- Short block (1 byte length)
            if offset + 1 >= length then break end
            header_size = 2
            payload_length = buffer(offset + 1, 1):uint()
        elseif len_type == 0x80 then
            -- Long block (2 bytes length, little-endian)
            if offset + 2 >= length then break end
            header_size = 3
            payload_length = buffer(offset + 1, 2):le_uint()
        else
            -- Reserved
            break
        end

        local frame_length = header_size + payload_length
        if offset + frame_length > length then break end

        -- Create subtree for this frame
        local cmd_name = command_names[cmd_type] or string.format("UNKNOWN (0x%02X)", cmd_type)
        local frame_tree = tree:add(cwnet_protocol, buffer(offset, frame_length),
                                     string.format("Frame %d: %s", frame_count, cmd_name))

        -- Add command byte details
        local cmd_tree = frame_tree:add(f_command, buffer(offset, 1), command_byte)
        cmd_tree:add(f_command_type, buffer(offset, 1), cmd_type):append_text(" (" .. cmd_name .. ")")
        cmd_tree:add(f_length_type, buffer(offset, 1), len_type)

        -- Add length field
        if len_type == 0x40 then
            frame_tree:add(f_length, buffer(offset + 1, 1), payload_length)
        elseif len_type == 0x80 then
            frame_tree:add(f_length, buffer(offset + 1, 2), payload_length)
        end

        -- Parse payload based on command type
        if payload_length > 0 then
            local payload_offset = offset + header_size
            frame_tree:add(f_payload, buffer(payload_offset, payload_length))

            if cmd_type == 0x01 then
                parse_connect(buffer, frame_tree, payload_offset, payload_length)
            elseif cmd_type == 0x03 then
                parse_ping(buffer, frame_tree, payload_offset, payload_length)
            elseif cmd_type == 0x04 then
                parse_print(buffer, frame_tree, payload_offset, payload_length)
            elseif cmd_type == 0x06 then
                parse_rigctld(buffer, frame_tree, payload_offset, payload_length)
            elseif cmd_type == 0x10 then
                parse_morse(buffer, frame_tree, payload_offset, payload_length)
            end
        end

        -- Collect command name for info column
        table.insert(info_text, cmd_name)

        offset = offset + frame_length
    end

    -- Update info column once at the end (safer for all Wireshark versions)
    if #info_text > 0 then
        pinfo.cols.info = table.concat(info_text, ", ")
    end
end

-- Register the protocol on TCP port 7355 (default CWNet port)
local tcp_port = DissectorTable.get("tcp.port")
tcp_port:add(7355, cwnet_protocol)

-- Also allow "Decode As..." for other ports
tcp_port:add(0, cwnet_protocol)
