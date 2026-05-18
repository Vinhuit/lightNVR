/**
 * DetectionTab — Detection models path, API URL / backend, default detection
 * threshold (used when a stream enables detection-based recording).
 *
 * Part of PRD UXD_01 §5.2 / T2 settings restructure (#399).
 */

export function DetectionTab({ settings, handleInputChange, handleThresholdChange, canModifySettings, t }) {
  return (
    <div class="space-y-6">
      <div class="settings-group bg-card text-card-foreground rounded-lg shadow p-4">
        <h3 class="text-lg font-semibold mb-4 pb-2 border-b border-border">{t('settings.detectionBasedRecording')}</h3>
        <div class="setting mb-4">
          <p class="setting-description mb-2 text-gray-700 dark:text-gray-300">
            {t('settings.detectionBasedRecordingDescription')}
          </p>
          <p class="setting-description mb-2 text-gray-700 dark:text-gray-300">
            <strong>{t('settings.motionDetectionLabel')}</strong> {t('settings.motionDetectionDescription')}
          </p>
          <p class="setting-description mb-2 text-gray-700 dark:text-gray-300">
            <strong>{t('settings.optimizedMotionDetectionLabel')}</strong> {t('settings.optimizedMotionDetectionDescription')}
          </p>
        </div>
        <div data-setting-label={t('settings.detectionModelsPath')} class="setting grid grid-cols-1 md:grid-cols-3 gap-4 items-center mb-4">
          <label for="setting-detection-models-path" class="font-medium">{t('settings.detectionModelsPath')}</label>
          <div class="col-span-2">
            <input
              type="text"
              id="setting-detection-models-path"
              name="detectionModelsPath"
              placeholder="/var/lib/lightnvr/models"
              class="w-full p-2 border border-input rounded bg-background text-foreground disabled:opacity-60 disabled:cursor-not-allowed"
              value={settings.detectionModelsPath}
              onChange={handleInputChange}
              disabled={!canModifySettings}
            />
            <span class="hint text-sm text-muted-foreground block mt-1">{t('settings.detectionModelsPathHelp')}</span>
          </div>
        </div>
        <div data-setting-label={t('settings.apiDetectionUrl')} class="setting grid grid-cols-1 md:grid-cols-3 gap-4 items-center mb-4">
          <label for="setting-api-detection-url" class="font-medium">{t('settings.apiDetectionUrl')}</label>
          <div class="col-span-2">
            <input
              type="text"
              id="setting-api-detection-url"
              name="apiDetectionUrl"
              class="p-2 border border-input rounded bg-background text-foreground w-full max-w-md disabled:opacity-60 disabled:cursor-not-allowed"
              value={settings.apiDetectionUrl}
              onChange={handleInputChange}
              disabled={!canModifySettings}
              placeholder="http://localhost:9001/api/v1/detect"
            />
            <span class="hint text-sm text-muted-foreground block mt-1">{t('settings.apiDetectionUrlHelp')}</span>
          </div>
        </div>
        <div data-setting-label={t('settings.apiDetectionBackend')} class="setting grid grid-cols-1 md:grid-cols-3 gap-4 items-center mb-4">
          <label for="setting-api-detection-backend" class="font-medium">{t('settings.apiDetectionBackend')}</label>
          <div class="col-span-2">
            <select
              id="setting-api-detection-backend"
              name="apiDetectionBackend"
              class="p-2 border border-input rounded bg-background text-foreground disabled:opacity-60 disabled:cursor-not-allowed"
              value={settings.apiDetectionBackend}
              onChange={handleInputChange}
              disabled={!canModifySettings}
            >
              <option value="onnx">{t('settings.apiDetectionBackendOnnx')}</option>
              <option value="tflite">{t('settings.apiDetectionBackendTflite')}</option>
              <option value="opencv">{t('settings.apiDetectionBackendOpencv')}</option>
            </select>
            <span class="hint text-sm text-muted-foreground block mt-1">{t('settings.apiDetectionBackendHelp')}</span>
          </div>
        </div>
        <div data-setting-label={t('settings.defaultDetectionThreshold')} class="setting grid grid-cols-1 md:grid-cols-3 gap-4 items-center mb-4">
          <label for="setting-default-detection-threshold" class="font-medium">{t('settings.defaultDetectionThreshold')}</label>
          <div class="col-span-2">
            <div class="flex items-center">
              <input
                type="range"
                id="setting-default-detection-threshold"
                name="defaultDetectionThreshold"
                min="0"
                max="100"
                step="1"
                class="w-full h-2 bg-secondary rounded-lg appearance-none cursor-pointer accent-primary disabled:opacity-60 disabled:cursor-not-allowed"
                value={settings.defaultDetectionThreshold}
                onChange={handleThresholdChange}
                disabled={!canModifySettings}
              />
              <span id="threshold-value" class="ml-2 min-w-[3rem] text-center">{settings.defaultDetectionThreshold}%</span>
            </div>
            <span class="hint text-sm text-muted-foreground block mt-1">{t('settings.defaultDetectionThresholdHelp')}</span>
          </div>
        </div>
        <div data-setting-label="Object class filter" class="setting grid grid-cols-1 md:grid-cols-3 gap-4 items-center mb-4">
          <label for="setting-api-detection-filter-classes" class="font-medium">Object class filter</label>
          <div class="col-span-2">
            <input
              type="text"
              id="setting-api-detection-filter-classes"
              name="apiDetectionFilterClasses"
              class="p-2 border border-input rounded bg-background text-foreground w-full max-w-md disabled:opacity-60 disabled:cursor-not-allowed"
              value={settings.apiDetectionFilterClasses}
              onChange={handleInputChange}
              disabled={!canModifySettings}
              placeholder="person,car,motorcycle"
            />
            <span class="hint text-sm text-muted-foreground block mt-1">Sent to compatible external detectors as filter_classes.</span>
          </div>
        </div>
      </div>

      <div class="settings-group bg-card text-card-foreground rounded-lg shadow p-4">
        <h3 class="text-lg font-semibold mb-4 pb-2 border-b border-border">Event Enrichment</h3>
        <div data-setting-label="GenAI event descriptions" class="setting grid grid-cols-1 md:grid-cols-3 gap-4 items-center mb-4">
          <label for="setting-genai-enabled" class="font-medium">GenAI descriptions</label>
          <div class="col-span-2">
            <label class="inline-flex items-center gap-2">
              <input
                type="checkbox"
                id="setting-genai-enabled"
                name="genaiEnabled"
                checked={settings.genaiEnabled}
                onChange={handleInputChange}
                disabled={!canModifySettings}
              />
              <span>Queue GenAI jobs</span>
            </label>
          </div>
        </div>
        <div data-setting-label="GenAI provider" class="setting grid grid-cols-1 md:grid-cols-3 gap-4 items-center mb-4">
          <label for="setting-genai-provider" class="font-medium">GenAI provider</label>
          <div class="col-span-2">
            <select
              id="setting-genai-provider"
              name="genaiProvider"
              class="p-2 border border-input rounded bg-background text-foreground w-full max-w-md disabled:opacity-60 disabled:cursor-not-allowed"
              value={settings.genaiProvider}
              onChange={handleInputChange}
              disabled={!canModifySettings}
            >
              <option value="external">External worker</option>
              <option value="openai">OpenAI / ChatGPT API</option>
              <option value="gemini">Google Gemini API</option>
              <option value="ollama">Ollama native API</option>
              <option value="openai_compatible">OpenAI-compatible API</option>
            </select>
          </div>
        </div>
        <div data-setting-label="GenAI API URL" class="setting grid grid-cols-1 md:grid-cols-3 gap-4 items-center mb-4">
          <label for="setting-genai-api-url" class="font-medium">GenAI API URL</label>
          <div class="col-span-2">
            <input
              type="text"
              id="setting-genai-api-url"
              name="genaiApiUrl"
              class="p-2 border border-input rounded bg-background text-foreground w-full max-w-md disabled:opacity-60 disabled:cursor-not-allowed"
              value={settings.genaiApiUrl}
              onChange={handleInputChange}
              disabled={!canModifySettings}
              placeholder={settings.genaiProvider === 'openai'
                ? 'https://api.openai.com/v1/responses'
                : settings.genaiProvider === 'gemini'
                  ? 'https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent'
                  : settings.genaiProvider === 'ollama'
                  ? 'http://localhost:11434/api/chat'
                  : settings.genaiProvider === 'openai_compatible'
                    ? 'http://localhost:11434/v1'
                    : 'http://localhost:9101/enrich'}
            />
            <span class="hint text-sm text-muted-foreground block mt-1">OpenAI, Gemini, and OpenAI-compatible chat APIs are processed by the built-in enrichment worker; other providers are queued for external workers.</span>
          </div>
        </div>
        <div data-setting-label="GenAI model" class="setting grid grid-cols-1 md:grid-cols-3 gap-4 items-center mb-4">
          <label for="setting-genai-model" class="font-medium">GenAI model</label>
          <div class="col-span-2">
            <input
              type="text"
              id="setting-genai-model"
              name="genaiModel"
              class="p-2 border border-input rounded bg-background text-foreground w-full max-w-md disabled:opacity-60 disabled:cursor-not-allowed"
              value={settings.genaiModel}
              onChange={handleInputChange}
              disabled={!canModifySettings}
              placeholder={settings.genaiProvider === 'ollama'
                ? 'llava'
                : settings.genaiProvider === 'gemini'
                  ? 'gemini-2.5-flash'
                  : 'gpt-5.4-mini'}
            />
          </div>
        </div>
        <div data-setting-label="GenAI API key environment variable" class="setting grid grid-cols-1 md:grid-cols-3 gap-4 items-center mb-4">
          <label for="setting-genai-api-key-env" class="font-medium">API key env</label>
          <div class="col-span-2">
            <input
              type="text"
              id="setting-genai-api-key-env"
              name="genaiApiKeyEnv"
              class="p-2 border border-input rounded bg-background text-foreground w-full max-w-md disabled:opacity-60 disabled:cursor-not-allowed"
              value={settings.genaiApiKeyEnv}
              onChange={handleInputChange}
              disabled={!canModifySettings}
              placeholder={settings.genaiProvider === 'gemini' ? 'GEMINI_API_KEY' : 'OPENAI_API_KEY'}
            />
            <span class="hint text-sm text-muted-foreground block mt-1">Store the secret in the environment; do not paste API keys into this field.</span>
          </div>
        </div>
        <div data-setting-label="Face recognition" class="setting grid grid-cols-1 md:grid-cols-3 gap-4 items-center mb-4">
          <label for="setting-face-recognition-enabled" class="font-medium">Face recognition</label>
          <div class="col-span-2">
            <label class="inline-flex items-center gap-2">
              <input
                type="checkbox"
                id="setting-face-recognition-enabled"
                name="faceRecognitionEnabled"
                checked={settings.faceRecognitionEnabled}
                onChange={handleInputChange}
                disabled={!canModifySettings}
              />
              <span>Queue face jobs</span>
            </label>
          </div>
        </div>
        <div data-setting-label="Face recognition API URL" class="setting grid grid-cols-1 md:grid-cols-3 gap-4 items-center mb-4">
          <label for="setting-face-recognition-api-url" class="font-medium">Face API URL</label>
          <div class="col-span-2">
            <input
              type="text"
              id="setting-face-recognition-api-url"
              name="faceRecognitionApiUrl"
              class="p-2 border border-input rounded bg-background text-foreground w-full max-w-md disabled:opacity-60 disabled:cursor-not-allowed"
              value={settings.faceRecognitionApiUrl}
              onChange={handleInputChange}
              disabled={!canModifySettings}
              placeholder="http://localhost:9201/recognize"
            />
          </div>
        </div>
      </div>
    </div>
  );
}

export default DetectionTab;
