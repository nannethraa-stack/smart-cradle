/*
 * Pluggable cry-pattern ML adapter.
 *
 * Set CRY_MODEL_URL to a model service trained from the curated audio dataset.
 * The service should accept:
 *   { audio_base64, format, sample_rate, device_id, event_id }
 * and return:
 *   { probable_pattern, probability, model_version, features? }
 *
 * No clinical diagnosis is produced here. "Probable pattern" is intentionally
 * the product-facing term.
 */
async function analyze(audioEvent) {
  const url = process.env.CRY_MODEL_URL;
  if (!url) {
    return {
      audio_event_id: audioEvent.event_id,
      device_id: audioEvent.device_id,
      occurred_at: audioEvent.occurred_at,
      status: 'not_configured',
      model_version: 'cry-pattern-model-not-configured',
      probable_pattern: 'Analysing...',
      probability: null,
      inference_ms: 0,
      features: {}
    };
  }

  const started = Date.now();
  try {
    const response = await fetch(url, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({
        audio_base64: audioEvent.data_base64,
        format: audioEvent.format,
        sample_rate: audioEvent.sample_rate,
        device_id: audioEvent.device_id,
        event_id: audioEvent.event_id
      })
    });
    if (!response.ok) throw new Error(`model service HTTP ${response.status}`);
    const result = await response.json();
    return {
      audio_event_id: audioEvent.event_id,
      device_id: audioEvent.device_id,
      occurred_at: audioEvent.occurred_at,
      status: 'inference_complete',
      model_version: result.model_version || 'external-cry-model',
      probable_pattern: result.probable_pattern || 'Analysing...',
      probability: Number.isFinite(result.probability) ? result.probability : null,
      inference_ms: Date.now() - started,
      features: result.features || {}
    };
  } catch (error) {
    return {
      audio_event_id: audioEvent.event_id,
      device_id: audioEvent.device_id,
      occurred_at: audioEvent.occurred_at,
      status: 'inference_error',
      model_version: 'external-cry-model',
      probable_pattern: 'Analysing...',
      probability: null,
      inference_ms: Date.now() - started,
      features: {},
      error: error.message
    };
  }
}
module.exports = { analyze };
