api = freeswitch.API()

local channel_id = session:get_uuid()

session:execute("set", "playback_delimiter=!")
session:execute("answer")
local caller_id_number = session:getVariable("caller_id_number")
session:execute("set", "result=${uuid_audio_stream ${uuid} start ws://10.25.0.4:8080/live/${uuid} mono 16000}")
freeswitch.consoleLog("INFO", "call answered with channel id [" .. channel_id .. "]")
session:execute("record_session", "/tmp/${uuid}.wav")
session:execute("park")
