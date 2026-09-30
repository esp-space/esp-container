// Python/C 合同回归向同一浏览器 SDK 传入独立生成的字节，不执行 guest。
import { decodePublicKeyPEM, verifyProductPackage } from '../tools/product-package.mjs'
let text=''
for await (const chunk of process.stdin) text+=chunk
const inputs=JSON.parse(text), results=[]
for(const input of inputs) {
  try {
    const value=await verifyProductPackage(new Uint8Array(Buffer.from(input.package,'base64')),{
      publicKeySPKI:decodePublicKeyPEM(input.public_key_pem),expectedKeyId:input.key_id,
    })
    results.push({accepted:true,package_sha256:value.packageSha256,manifest:value.manifest})
  } catch { results.push({accepted:false}) }
}
process.stdout.write(JSON.stringify(results))
