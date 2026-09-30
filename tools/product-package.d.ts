export type ProductManifest = Readonly<{
  package_format_version: 1
  product_id: string
  product_version: string
  guest_abi_version: 2
  required_capabilities: readonly ('monotonic-time' | 'log' | 'timer')[]
  runtime_profile: 'wamr-classic-v1'
  limits: Readonly<{ memory_limit_bytes: 65536; stack_limit_bytes: number; event_queue_limit: number;
    instruction_budget: number; host_call_timeout_ms: number; storage_limit_bytes: number }>
  data_schema_version: number
  payload: Readonly<{ path: 'app.wasm'; size_bytes: number; sha256: string }>
  signature_algorithm: 'rsa-3072-pss-sha256'
  signing_key_id: string
}>
export type VerifiedProductPackage = Readonly<{ manifest: ProductManifest; packageSha256: string;
  packageSizeBytes: number; signingKeyFingerprintSha256: string }>
export type ProductPackageFailure = 'invalid_package' | 'invalid_trust_anchor' | 'signature_mismatch' | 'unsupported_environment'
export class ProductPackageError extends Error { readonly reason: ProductPackageFailure }
export function decodePublicKeyPEM(text: string): Uint8Array
export function verifyProductPackage(input: Uint8Array, options: { publicKeySPKI: Uint8Array;
  expectedKeyId: string; maxWasmBytes?: number; maxPackageBytes?: number }): Promise<VerifiedProductPackage>
