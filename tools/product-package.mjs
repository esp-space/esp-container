// product.pkg v1 的浏览器/Node 主机验包入口；与公开 Python/C 检查同一字节合同。
const MANIFEST_MAX_BYTES = 4096
const MAX_WASM_BYTES = 512 * 1024
const MAX_PACKAGE_BYTES = MAX_WASM_BYTES + MANIFEST_MAX_BYTES + 8192
const MEMORY_BYTES = 65536
const EVENT_BUFFER_BYTES = 4096
const IDENTIFIER = /^[a-z0-9]+(?:-[a-z0-9]+)*(?![\s\S])/
const DIGEST = /^[0-9a-f]{64}(?![\s\S])/
const encoder = new TextEncoder()
const decoder = new TextDecoder('utf-8', { fatal: true })
const MANIFEST_KEYS = ['package_format_version', 'product_id', 'product_version', 'guest_abi_version',
  'required_capabilities', 'runtime_profile', 'limits', 'data_schema_version', 'payload', 'signature_algorithm', 'signing_key_id']
const LIMIT_KEYS = ['memory_limit_bytes', 'stack_limit_bytes', 'event_queue_limit', 'instruction_budget', 'host_call_timeout_ms', 'storage_limit_bytes']

export class ProductPackageError extends Error {
  constructor(reason) { super(reason); this.name = 'ProductPackageError'; this.reason = reason }
}
function invalid(reason = 'invalid_package') { throw new ProductPackageError(reason) }
function equal(left, right) { return left.length === right.length && left.every((byte, index) => byte === right[index]) }
function object(value, keys) {
  if (value === null || typeof value !== 'object' || Array.isArray(value) ||
      Object.keys(value).length !== keys.length || !keys.every(key => Object.hasOwn(value, key))) invalid()
  return value
}
function integer(value, minimum = 1, maximum = 0xffffffff) {
  if (!Number.isInteger(value) || value < minimum || value > maximum) invalid()
}
function canonical(value) {
  if (Array.isArray(value)) return value.map(canonical)
  if (value !== null && typeof value === 'object') {
    return Object.fromEntries(Object.keys(value).sort().map(key => [key, canonical(value[key])]))
  }
  return value
}
function manifest(bytes) {
  if (!bytes.length || bytes.length > MANIFEST_MAX_BYTES) invalid()
  let value
  try { value = JSON.parse(decoder.decode(bytes)) } catch { invalid() }
  object(value, MANIFEST_KEYS)
  // 规范字节比对也拒绝重复 JSON key、替代转义、非规范数字与尾随内容。
  if (!equal(bytes, encoder.encode(JSON.stringify(canonical(value))))) invalid()
  if (value.package_format_version !== 1 || value.guest_abi_version !== 2 ||
      value.runtime_profile !== 'wamr-classic-v1' || value.signature_algorithm !== 'rsa-3072-pss-sha256') invalid()
  for (const key of ['product_id', 'product_version', 'runtime_profile', 'signing_key_id']) {
    if (typeof value[key] !== 'string' || !IDENTIFIER.test(value[key])) invalid()
  }
  integer(value.data_schema_version)
  const caps = value.required_capabilities
  if (!Array.isArray(caps) || caps.length > 16 ||
      caps.some(cap => typeof cap !== 'string' || !IDENTIFIER.test(cap) || !['monotonic-time', 'log', 'timer'].includes(cap)) ||
      JSON.stringify(caps) !== JSON.stringify([...new Set(caps)].sort())) invalid()
  object(value.limits, LIMIT_KEYS)
  for (const key of LIMIT_KEYS) integer(value.limits[key], key === 'storage_limit_bytes' ? 0 : 1)
  if (value.limits.memory_limit_bytes !== MEMORY_BYTES) invalid()
  object(value.payload, ['path', 'size_bytes', 'sha256'])
  if (value.payload.path !== 'app.wasm' || typeof value.payload.sha256 !== 'string' || !DIGEST.test(value.payload.sha256)) invalid()
  integer(value.payload.size_bytes, 8, MAX_WASM_BYTES)
  return value
}
function header(name, size) {
  const result = new Uint8Array(512)
  const put = (offset, text) => result.set(encoder.encode(text), offset)
  put(0, name); put(100, '0000644\0'); put(108, '0000000\0'); put(116, '0000000\0')
  put(124, size.toString(8).padStart(11, '0') + '\0'); put(136, '00000000000\0')
  result.fill(32, 148, 156); result[156] = 48; put(257, 'ustar\0' + '00')
  put(148, result.reduce((sum, byte) => sum + byte, 0).toString(8).padStart(6, '0') + '\0 ')
  return result
}
function unpack(bytes, maxWasmBytes) {
  let offset = 0
  const members = []
  for (const [name, maximum] of [['manifest.json', MANIFEST_MAX_BYTES], ['signature.bin', 384], ['app.wasm', maxWasmBytes]]) {
    if (offset + 512 > bytes.length) invalid()
    const actual = bytes.subarray(offset, offset + 512)
    const rawSize = String.fromCharCode(...actual.subarray(124, 136))
    if (!/^[0-7]{11}\0$/.test(rawSize)) invalid()
    const size = Number.parseInt(rawSize.slice(0, 11), 8)
    if (size > maximum || !equal(actual, header(name, size))) invalid()
    offset += 512
    if (offset + size > bytes.length) invalid()
    members.push(bytes.subarray(offset, offset + size)); offset += size
    const pad = (-size % 512 + 512) % 512
    if (bytes.subarray(offset, offset + pad).some(byte => byte !== 0)) invalid()
    offset += pad
  }
  if (bytes.length !== Math.ceil((offset + 1024) / 10240) * 10240 ||
      bytes.subarray(offset).some(byte => byte !== 0)) invalid()
  return members
}

class WasmReader {
  constructor(bytes) { this.bytes = bytes; this.offset = 0 }
  take(size) {
    if (size > this.bytes.length - this.offset) invalid()
    const value = this.bytes.subarray(this.offset, this.offset + size); this.offset += size
    return value
  }
  byte() { return this.take(1)[0] }
  u32() {
    let value = 0
    for (let index = 0; index < 5; index++) {
      const byte = this.byte()
      if (index === 4 && (byte & 0xf0)) invalid()
      value += (byte & 0x7f) * 2 ** (7 * index)
      if (!(byte & 0x80)) return value
    }
    invalid()
  }
  name() { return this.take(this.u32()) }
  nameText() { try { return decoder.decode(this.name()) } catch { invalid() } }
  signed(bits) {
    let value = 0n
    for (let index = 0; index < Math.ceil(bits / 7); index++) {
      const byte = this.byte(), shift = index * 7
      if (index === Math.ceil(bits / 7) - 1) {
        const mask = (0x7f << (bits - shift - 1)) & 0x7f
        if ((byte & mask) !== 0 && (byte & mask) !== mask) invalid()
      }
      value |= BigInt(byte & 0x7f) << BigInt(shift)
      if (!(byte & 0x80)) return byte & 0x40 ? value | (-1n << BigInt(shift + 7)) : value
    }
    invalid()
  }
  finish() { if (this.offset !== this.bytes.length) invalid() }
  count() { const count = this.u32(); if (count > this.bytes.length - this.offset) invalid(); return count }
}
function signatureEqual(actual, params, result) { return equal(actual[0], params) && equal(actual[1], result) }
function checkWasm(bytes, record) {
  if (!equal(bytes.subarray(0, 8), new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]))) invalid()
  const scan = new WasmReader(bytes.subarray(8)), sections = new Map()
  let last = 0
  while (scan.offset < scan.bytes.length) {
    const section = scan.byte(), content = scan.take(scan.u32())
    if ([4, 8, 9].includes(section) || section > 12) invalid()
    if (section === 0) {
      if (equal(new WasmReader(content).name(), encoder.encode('target_features'))) invalid()
    } else {
      if (section <= last) invalid()
      sections.set(section, content); last = section
    }
  }
  if (![1, 3, 5, 6, 7, 10].every(section => sections.has(section))) invalid()
  const types = [], typeReader = new WasmReader(sections.get(1)), typeCount = typeReader.count()
  for (let index = 0; index < typeCount; index++) {
    if (typeReader.byte() !== 0x60) invalid()
    const params = typeReader.take(typeReader.u32()), results = typeReader.take(typeReader.u32())
    if ([...params, ...results].some(value => ![0x7f, 0x7e, 0x7d, 0x7c].includes(value))) invalid()
    types.push([params, results])
  }
  typeReader.finish()
  let importedCount = 0
  const required = new Set()
  if (sections.has(2)) {
    const input = new WasmReader(sections.get(2)), count = input.u32(), seen = new Set()
    const expected = { monotonic_ms: [[], [0x7e], 'monotonic-time'], log: [[0x7f, 0x7f], [0x7f], 'log'],
      timer_start: [[0x7f, 0x7f], [0x7e], 'timer'], timer_cancel: [[0x7e], [0x7f], 'timer'] }
    if (count > 4) invalid()
    for (let index = 0; index < count; index++) {
      const module = input.nameText(), field = input.nameText(), kind = input.byte(), type = input.u32()
      if (module !== 'econtainer' || kind !== 0 || !Object.hasOwn(expected, field) || seen.has(field) || type >= types.length ||
          !signatureEqual(types[type], expected[field][0], expected[field][1])) invalid()
      seen.add(field); required.add(expected[field][2])
    }
    input.finish(); importedCount = count
  }
  const functionsReader = new WasmReader(sections.get(3)), functions = [], functionCount = functionsReader.count()
  for (let index = 0; index < functionCount; index++) {
    const type = functionsReader.u32(); if (type >= types.length) invalid(); functions.push(type)
  }
  functionsReader.finish()
  const exportReader = new WasmReader(sections.get(7)), exports = new Map()
  const wanted = { econtainer_init: 0, econtainer_on_event: 0, econtainer_stop: 0, memory: 2, econtainer_event_buffer: 3 }
  if (exportReader.u32() !== 5) invalid()
  for (let index = 0; index < 5; index++) {
    const name = exportReader.nameText(), kind = exportReader.byte(), number = exportReader.u32()
    if (!Object.hasOwn(wanted, name) || wanted[name] !== kind || exports.has(name) || name === 'memory' && number !== 0) invalid()
    exports.set(name, number)
  }
  exportReader.finish()
  const entries = ['econtainer_init', 'econtainer_on_event', 'econtainer_stop'].map(name => exports.get(name))
  if (new Set(entries).size !== 3) invalid()
  for (const [position, entry] of entries.entries()) {
    if (entry < importedCount || entry - importedCount >= functions.length ||
        !signatureEqual(types[functions[entry - importedCount]], position === 1 ? [0x7f, 0x7f] : [], [0x7f])) invalid()
  }
  const globals = new WasmReader(sections.get(6)), globalCount = globals.count(), eventGlobal = exports.get('econtainer_event_buffer')
  if (eventGlobal >= globalCount) invalid()
  for (let index = 0; index < globalCount; index++) {
    const kind = globals.byte(), mutable = globals.byte(), opcode = globals.byte()
    if (mutable !== 0 && mutable !== 1) invalid()
    let value = 0n
    if (kind === 0x7f && opcode === 0x41 || kind === 0x7e && opcode === 0x42) value = globals.signed(kind === 0x7f ? 32 : 64)
    else if (kind === 0x7d && opcode === 0x43 || kind === 0x7c && opcode === 0x44) globals.take(kind === 0x7d ? 4 : 8)
    else invalid()
    if (globals.byte() !== 0x0b || index === eventGlobal &&
        (kind !== 0x7f || mutable !== 0 || value <= 0n || value > BigInt(MEMORY_BYTES - EVENT_BUFFER_BYTES))) invalid()
  }
  globals.finish()
  const memory = new WasmReader(sections.get(5)), count = memory.u32(), flags = memory.u32(), minimum = memory.u32(), maximum = memory.u32()
  memory.finish()
  if (count !== 1 || flags !== 1 || minimum !== 1 || maximum !== 1) invalid()
  const code = new WasmReader(sections.get(10))
  if (code.u32() !== functions.length) invalid()
  for (let index = 0; index < functions.length; index++) {
    const body = code.take(code.u32()); if (body.length < 2 || body.at(-1) !== 0x0b) invalid()
  }
  code.finish()
  if ([...required].some(cap => !record.required_capabilities.includes(cap))) invalid()
}

function freeze(value) {
  if (value !== null && typeof value === 'object') { Object.values(value).forEach(freeze); Object.freeze(value) }
  return value
}
export function decodePublicKeyPEM(text) {
  if (typeof text !== 'string' || text.length > 8192) invalid('invalid_trust_anchor')
  const match = /^-----BEGIN PUBLIC KEY-----\r?\n([A-Za-z0-9+/=\r\n]+)-----END PUBLIC KEY-----\s*$/.exec(text)
  if (!match) invalid('invalid_trust_anchor')
  const base64 = match[1].replace(/[\r\n]/g, '')
  if (!/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(base64)) invalid('invalid_trust_anchor')
  try { return Uint8Array.from(atob(base64), character => character.charCodeAt(0)) } catch { invalid('invalid_trust_anchor') }
}
async function digest(bytes) {
  return [...new Uint8Array(await globalThis.crypto.subtle.digest('SHA-256', bytes))]
    .map(byte => byte.toString(16).padStart(2, '0')).join('')
}

export async function verifyProductPackage(input, options) {
  if (!globalThis.crypto?.subtle) invalid('unsupported_environment')
  const maxWasmBytes = options?.maxWasmBytes ?? MAX_WASM_BYTES
  const maxPackageBytes = options?.maxPackageBytes ?? MAX_PACKAGE_BYTES
  integer(maxWasmBytes, 8, MAX_WASM_BYTES); integer(maxPackageBytes, 10240, MAX_PACKAGE_BYTES)
  if (!(input instanceof Uint8Array) || input.byteLength > maxPackageBytes || input.byteLength % 512) invalid()
  if (typeof options?.expectedKeyId !== 'string' || !IDENTIFIER.test(options.expectedKeyId) ||
      !(options?.publicKeySPKI instanceof Uint8Array) || options.publicKeySPKI.byteLength > 8192) invalid('invalid_trust_anchor')
  // await 前冻结输入，调用者变更 buffer、options 或 key 不能改变本次验证对象。
  const bytes = Uint8Array.from(input), keyBytes = Uint8Array.from(options.publicKeySPKI), expectedKeyId = options.expectedKeyId
  const [manifestBytes, signature, wasm] = unpack(bytes, maxWasmBytes), record = manifest(manifestBytes)
  if (record.signing_key_id !== expectedKeyId) invalid('invalid_trust_anchor')
  if (signature.length !== 384) invalid()
  let key
  try {
    key = await globalThis.crypto.subtle.importKey('spki', keyBytes, { name: 'RSA-PSS', hash: 'SHA-256' }, false, ['verify'])
  } catch { invalid('invalid_trust_anchor') }
  if (key.algorithm.modulusLength !== 3072) invalid('invalid_trust_anchor')
  const domain = encoder.encode('ESP-CONTAINER-PRODUCT-V1\0'), signed = new Uint8Array(domain.length + manifestBytes.length)
  signed.set(domain); signed.set(manifestBytes, domain.length)
  if (!await globalThis.crypto.subtle.verify({ name: 'RSA-PSS', saltLength: 32 }, key, signature, signed)) invalid('signature_mismatch')
  if (record.payload.size_bytes !== wasm.length || record.payload.sha256 !== await digest(wasm)) invalid()
  checkWasm(wasm, record)
  return freeze({ manifest: record, packageSha256: await digest(bytes), packageSizeBytes: bytes.length,
    signingKeyFingerprintSha256: await digest(keyBytes) })
}
