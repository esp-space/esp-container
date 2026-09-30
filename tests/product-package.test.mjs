import assert from 'node:assert/strict'
import { createHash, generateKeyPairSync, sign, constants } from 'node:crypto'
import { readFile } from 'node:fs/promises'
import { test } from 'node:test'
import { decodePublicKeyPEM, ProductPackageError, verifyProductPackage } from '../tools/product-package.mjs'

const vector = new Uint8Array(await readFile(new URL('./vectors/product-v1/product.pkg', import.meta.url)))
const pem = await readFile(new URL('./vectors/product-v1/public-key.pem', import.meta.url), 'utf8')
const options = () => ({ publicKeySPKI: decodePublicKeyPEM(pem), expectedKeyId: 'test-key' })
const hash = bytes => createHash('sha256').update(bytes).digest('hex')
const manifest = JSON.parse(new TextDecoder().decode(vector.subarray(512, 512 + 546)))
const wasm = vector.slice(3072, 3072 + 171)
// 原向量的三个 tarfile 规范 header 作为独立输入，只改长度与 checksum。
function archive(m, s, w) {
  const parts = []
  for (const [original, payload] of [[0, m], [1536, s], [2560, w]]) {
    const header = vector.slice(original, original + 512)
    header.set(new TextEncoder().encode(payload.length.toString(8).padStart(11, '0') + '\0'), 124)
    header.fill(32, 148, 156)
    header.set(new TextEncoder().encode(header.reduce((sum, byte) => sum + byte, 0).toString(8).padStart(6, '0') + '\0 '), 148)
    parts.push(header, payload, new Uint8Array((512 - payload.length % 512) % 512))
  }
  const length = parts.reduce((total, part) => total + part.length, 0)
  const result = new Uint8Array(Math.ceil((length + 1024) / 10240) * 10240)
  let offset = 0
  for (const part of parts) { result.set(part, offset); offset += part.length }
  return result
}
const sorted = value => Array.isArray(value) ? value.map(sorted) :
  value !== null && typeof value === 'object' ? Object.fromEntries(Object.keys(value).sort().map(key => [key, sorted(value[key])])) : value
const pair = generateKeyPairSync('rsa', { modulusLength: 3072 })
const generatedOptions = { publicKeySPKI: new Uint8Array(pair.publicKey.export({ type: 'spki', format: 'der' })), expectedKeyId: 'test-key' }
function signed(record = manifest, body = wasm, saltLength = 32, exactManifest) {
  const raw = exactManifest ?? new TextEncoder().encode(JSON.stringify(sorted(record)))
  const signature = sign('sha256', Buffer.concat([Buffer.from('ESP-CONTAINER-PRODUCT-V1\0'), Buffer.from(raw)]), {
    key: pair.privateKey, padding: constants.RSA_PKCS1_PSS_PADDING, saltLength,
  })
  return archive(raw, signature, body)
}
const reject = (bytes, opts = options(), reason) => assert.rejects(verifyProductPackage(bytes, opts),
  error => error instanceof ProductPackageError && (reason === undefined || error.reason === reason))

test('公开 RSA-3072/PSS 向量得到完整签名元数据和精确包摘要', async () => {
  const value = await verifyProductPackage(vector, options())
  assert.equal(value.packageSha256, 'c41930e65d577133a09e7f2105f83faafd63e6d40a3c0af0e3d70aba58907702')
  assert.equal(value.packageSizeBytes, 10240)
  assert.equal(value.manifest.payload.sha256, hash(wasm))
  assert.equal(value.manifest.guest_abi_version, 2)
  assert.equal(value.manifest.product_version, 'v0-1-0')
  assert.equal(value.signingKeyFingerprintSha256, hash(options().publicKeySPKI))
  assert(Object.isFrozen(value) && Object.isFrozen(value.manifest.limits))
})
test('await 前复制包、公钥与预期 key ID，调用方变更不会偷换验证对象', async () => {
  const input = vector.slice(), opts = options()
  const result = verifyProductPackage(input, opts)
  input.fill(0); opts.publicKeySPKI.fill(0); opts.expectedKeyId = 'changed'
  assert.equal((await result).packageSha256, hash(vector))
})
test('错公钥、key ID、RSA 位数、PSS salt 和原签名拒绝', async () => {
  await reject(vector, { ...options(), expectedKeyId: 'wrong' }, 'invalid_trust_anchor')
  await reject(vector, { ...generatedOptions }, 'signature_mismatch')
  const small = generateKeyPairSync('rsa', { modulusLength: 2048 })
  await reject(vector, { ...options(), publicKeySPKI: new Uint8Array(small.publicKey.export({type:'spki',format:'der'})) }, 'invalid_trust_anchor')
  await reject(signed(manifest, wasm, 20), generatedOptions, 'signature_mismatch')
  const broken = vector.slice(); broken[2048] ^= 1
  await reject(broken, options(), 'signature_mismatch')
})
test('不从包名、JSON 或非独立材料生成验证信任锚', async () => {
  for (const input of ['', '-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----', pem+'extra', 'a'.repeat(8193)]) {
    assert.throws(()=>decodePublicKeyPEM(input),error=>error.reason==='invalid_trust_anchor')
  }
  await reject(vector, { ...options(), expectedKeyId:'test-key\n' }, 'invalid_trust_anchor')
})
test('规范 ustar 路径、类型、checksum、填充、结束块与长度不能被伪造', async () => {
  for (const [offset, value] of [[0, 47], [100, 49], [156, 50], [257, 88], [148, 49], [1058, 1], [500, 1], [3300, 1], [10239, 1]]) {
    const bytes = vector.slice(); bytes[offset] = value; await reject(bytes)
  }
  await reject(vector.slice(0, -512))
  const padded = new Uint8Array(vector.length+10240); padded.set(vector); await reject(padded)
  await reject(vector, { ...options(), maxWasmBytes:170 })
  await reject(vector, { ...options(), maxPackageBytes:10239 })
})
test('即使重新合法签名，也拒绝重复 key、非规范编码和清单越界', async () => {
  const text = JSON.stringify(sorted(manifest))
  for (const raw of [text.replace('"product_id":"counter"','"product_id":"counter","product_id":"counter"'), JSON.stringify(manifest,null,2),
    text.replace('"data_schema_version":1','"data_schema_version":1.0')]) {
    await reject(signed(manifest,wasm,32,new TextEncoder().encode(raw)),generatedOptions)
  }
  const cases = [
    { ...manifest,product_id:'counter\n' }, { ...manifest,product_version:'v0.1.0' }, { ...manifest,extra:0 },
    { ...manifest,guest_abi_version:3 }, { ...manifest,data_schema_version:true }, { ...manifest,required_capabilities:['timer','log'] },
    { ...manifest,required_capabilities:['timer','timer'] }, { ...manifest,required_capabilities:['gpio'] },
    { ...manifest,limits:{...manifest.limits,memory_limit_bytes:131072} },
    { ...manifest,payload:{...manifest.payload,path:'../app.wasm'} },
    { ...manifest,payload:{...manifest.payload,size_bytes:172} },
  ]
  for(const record of cases) await reject(signed(record),generatedOptions)
})
test('载荷摘要与 Wasm ABI、section、入口和事件区独立核对', async () => {
  const damaged = vector.slice(); damaged[3072] ^= 1; await reject(damaged)
  const fields = [
    bytes=>{ bytes[0]=1 }, bytes=>{ bytes[35]=4 },
    bytes=>{ bytes[bytes.length-1]=0 },
    bytes=>{ const index=Buffer.from(bytes).indexOf(Buffer.from('econtainer_stop')); bytes[index]=120 },
  ]
  for(const mutate of fields) {
    const body=wasm.slice(); mutate(body)
    const record={...manifest,payload:{...manifest.payload,sha256:hash(body)}}
    await reject(signed(record,body),generatedOptions)
  }
  const valid = await verifyProductPackage(signed(),generatedOptions)
  assert.equal(valid.manifest.product_id,'counter')
})
