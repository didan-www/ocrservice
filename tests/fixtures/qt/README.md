# Qt contract fixture provenance

These fixtures are extracted from the legal and illegal cases in the completed Qt client:

- `tests/http/tst_json_codecs.cpp`
- `tests/mqtt/tst_recognition_event_codec.cpp`
- `src/infrastructure/http/JsonCodecs.cpp`
- `src/infrastructure/mqtt/RecognitionEventCodec.cpp`

The values use the server's current REQ-001 UUID and exact millisecond time constraints. The
field sets, null combinations, safe-integer cases, pagination cases, login expiry pairing and
management-event `gateAction` rejection mirror the Qt codecs. Update both sides together when a
public contract change has first been approved in REQ-001.
