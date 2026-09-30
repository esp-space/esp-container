package productpkg

import (
	"bytes"
	"unicode/utf8"
)

type wasmReader struct {
	data   []byte
	offset int
	failed bool
}

func (r *wasmReader) take(size uint32) []byte {
	if r.failed || uint64(size) > uint64(len(r.data)-r.offset) {
		r.failed = true
		return nil
	}
	value := r.data[r.offset : r.offset+int(size)]
	r.offset += int(size)
	return value
}
func (r *wasmReader) byte() byte {
	value := r.take(1)
	if len(value) == 0 {
		return 0
	}
	return value[0]
}
func (r *wasmReader) u32() uint32 {
	var value uint32
	for i := 0; i < 5; i++ {
		item := r.byte()
		if r.failed || i == 4 && item&0xf0 != 0 {
			r.failed = true
			return 0
		}
		value |= uint32(item&0x7f) << (7 * i)
		if item&0x80 == 0 {
			return value
		}
	}
	r.failed = true
	return 0
}
func (r *wasmReader) count() uint32 {
	value := r.u32()
	if uint64(value) > uint64(len(r.data)-r.offset) {
		r.failed = true
		return 0
	}
	return value
}
func (r *wasmReader) name() []byte { return r.take(r.u32()) }
func (r *wasmReader) nameText() string {
	value := r.name()
	if !utf8.Valid(value) {
		r.failed = true
		return ""
	}
	return string(value)
}
func (r *wasmReader) signed(bits int) int64 {
	var value uint64
	for i := 0; i < (bits+6)/7; i++ {
		item, shift := r.byte(), 7*i
		if r.failed {
			return 0
		}
		if i == (bits+6)/7-1 {
			mask := byte((uint32(0x7f) << (bits - shift - 1)) & 0x7f)
			if item&mask != 0 && item&mask != mask {
				r.failed = true
				return 0
			}
		}
		value |= uint64(item&0x7f) << shift
		if item&0x80 == 0 {
			if item&0x40 != 0 && shift+7 < 64 {
				value |= ^uint64(0) << (shift + 7)
			}
			return int64(value)
		}
	}
	r.failed = true
	return 0
}
func (r *wasmReader) done() bool { return !r.failed && r.offset == len(r.data) }

type functionType struct{ params, results []byte }

func (t functionType) matches(params, results []byte) bool {
	return bytes.Equal(t.params, params) && bytes.Equal(t.results, results)
}

// checkWasm mirrors the existing host/device static profile. It deliberately
// does not replace WAMR instruction validation, linking or platform admission.
func checkWasm(wasm []byte, manifest Manifest) bool {
	if len(wasm) < 8 || !bytes.Equal(wasm[:8], []byte{0, 97, 115, 109, 1, 0, 0, 0}) {
		return false
	}
	scan := wasmReader{data: wasm[8:]}
	sections := make(map[byte][]byte)
	var last byte
	for scan.offset < len(scan.data) && !scan.failed {
		section := scan.byte()
		content := scan.take(scan.u32())
		if scan.failed || section == 4 || section == 8 || section == 9 || section > 12 {
			return false
		}
		if section == 0 {
			custom := wasmReader{data: content}
			name := custom.name()
			if custom.failed || bytes.Equal(name, []byte("target_features")) {
				return false
			}
		} else {
			if section <= last {
				return false
			}
			sections[section] = content
			last = section
		}
	}
	if !scan.done() {
		return false
	}
	for _, section := range []byte{1, 3, 5, 6, 7, 10} {
		if _, ok := sections[section]; !ok {
			return false
		}
	}
	input := wasmReader{data: sections[1]}
	types := make([]functionType, 0)
	for remaining := input.count(); remaining > 0 && !input.failed; remaining-- {
		if input.byte() != 0x60 {
			return false
		}
		params := input.take(input.u32())
		results := input.take(input.u32())
		for _, value := range append(bytes.Clone(params), results...) {
			if value < 0x7c || value > 0x7f {
				return false
			}
		}
		types = append(types, functionType{params: params, results: results})
	}
	if !input.done() {
		return false
	}
	expected := map[string]struct {
		functionType
		capability string
	}{
		"monotonic_ms": {functionType{nil, []byte{0x7e}}, "monotonic-time"},
		"log":          {functionType{[]byte{0x7f, 0x7f}, []byte{0x7f}}, "log"},
		"timer_start":  {functionType{[]byte{0x7f, 0x7f}, []byte{0x7e}}, "timer"},
		"timer_cancel": {functionType{[]byte{0x7e}, []byte{0x7f}}, "timer"},
	}
	var importedCount uint32
	required := make(map[string]bool)
	if section, ok := sections[2]; ok {
		input = wasmReader{data: section}
		importedCount = input.u32()
		if importedCount > 4 {
			return false
		}
		seen := make(map[string]bool)
		for i := uint32(0); i < importedCount; i++ {
			module, field := input.nameText(), input.nameText()
			kind, index := input.byte(), input.u32()
			want, ok := expected[field]
			if input.failed || module != "econtainer" || kind != 0 || !ok || seen[field] ||
				uint64(index) >= uint64(len(types)) || !types[index].matches(want.params, want.results) {
				return false
			}
			seen[field] = true
			required[want.capability] = true
		}
		if !input.done() {
			return false
		}
	}
	input = wasmReader{data: sections[3]}
	functions := make([]uint32, 0)
	for remaining := input.count(); remaining > 0 && !input.failed; remaining-- {
		index := input.u32()
		if uint64(index) >= uint64(len(types)) {
			return false
		}
		functions = append(functions, index)
	}
	if !input.done() {
		return false
	}
	input = wasmReader{data: sections[7]}
	wanted := map[string]byte{"econtainer_init": 0, "econtainer_on_event": 0, "econtainer_stop": 0, "memory": 2, "econtainer_event_buffer": 3}
	exports := make(map[string]uint32)
	if input.u32() != 5 {
		return false
	}
	for i := 0; i < 5; i++ {
		name := input.nameText()
		kind, index := input.byte(), input.u32()
		want, ok := wanted[name]
		_, duplicate := exports[name]
		if input.failed || !ok || kind != want || duplicate || name == "memory" && index != 0 {
			return false
		}
		exports[name] = index
	}
	if !input.done() {
		return false
	}
	entries := []uint32{exports["econtainer_init"], exports["econtainer_on_event"], exports["econtainer_stop"]}
	if entries[0] == entries[1] || entries[0] == entries[2] || entries[1] == entries[2] {
		return false
	}
	for position, index := range entries {
		if index < importedCount || uint64(index-importedCount) >= uint64(len(functions)) {
			return false
		}
		var params []byte
		if position == 1 {
			params = []byte{0x7f, 0x7f}
		}
		if !types[functions[index-importedCount]].matches(params, []byte{0x7f}) {
			return false
		}
	}
	input = wasmReader{data: sections[6]}
	globalCount, eventGlobal := input.count(), exports["econtainer_event_buffer"]
	if eventGlobal >= globalCount {
		return false
	}
	for i := uint32(0); i < globalCount; i++ {
		kind, mutable, opcode := input.byte(), input.byte(), input.byte()
		if mutable > 1 {
			return false
		}
		var value int64
		switch {
		case kind == 0x7f && opcode == 0x41:
			value = input.signed(32)
		case kind == 0x7e && opcode == 0x42:
			value = input.signed(64)
		case kind == 0x7d && opcode == 0x43:
			input.take(4)
		case kind == 0x7c && opcode == 0x44:
			input.take(8)
		default:
			return false
		}
		if input.byte() != 0x0b || input.failed || i == eventGlobal &&
			(kind != 0x7f || mutable != 0 || value <= 0 || value > MemoryBytes-EventBufferBytes) {
			return false
		}
	}
	if !input.done() {
		return false
	}
	input = wasmReader{data: sections[5]}
	count, flags, minimum, maximum := input.u32(), input.u32(), input.u32(), input.u32()
	if !input.done() || count != 1 || flags != 1 || minimum != 1 || maximum != 1 {
		return false
	}
	input = wasmReader{data: sections[10]}
	if uint64(input.u32()) != uint64(len(functions)) {
		return false
	}
	for range functions {
		body := input.take(input.u32())
		if len(body) < 2 || body[len(body)-1] != 0x0b {
			return false
		}
	}
	if !input.done() {
		return false
	}
	for capability := range required {
		found := false
		for _, granted := range manifest.RequiredCapabilities {
			if capability == granted {
				found = true
			}
		}
		if !found {
			return false
		}
	}
	return true
}
