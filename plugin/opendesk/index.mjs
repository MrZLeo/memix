import fs from 'node:fs';
import path from 'node:path';

const bundledPlatforms = new Map([
  ['darwin/arm64', ['macos-arm64', 'memix']],
  ['linux/x64', ['linux-x86_64', 'memix']],
  ['win32/x64', ['windows-x86_64', 'memix.exe']]
]);

const nativeMemoryWorkflows = new Map([
  ['memory-addMemory', 'save'],
  ['memory-updateMemory', 'save'],
  ['memory-removeMemory', 'save'],
  ['memory-searchMemory', 'recall'],
  ['memory-getMemory', 'recall']
]);

function memoryRedirect(tool) {
  const workflow = nativeMemoryWorkflows.get(tool);
  if (!workflow) return undefined;
  return [
    'The Memix plugin is active and is the authoritative durable-memory backend.',
    `Do not use the OpenDesk native tool ${tool}.`,
    'Load the pluginMgr/memix-cli skill, run `memix skill show`, and follow',
    `the \`memix agent ${workflow}\` workflow instead.`
  ].join(' ');
}

export function bundledBinary(pluginDir, platform = process.platform, arch = process.arch) {
  const selection = bundledPlatforms.get(`${platform}/${arch}`);
  if (!selection) {
    throw new Error(`Memix does not publish an OpenDesk binary for ${platform}/${arch}`);
  }
  const [directory, filename] = selection;
  const binary = path.join(pluginDir, 'bin', directory, filename);
  let status;
  try {
    status = fs.statSync(binary);
  } catch {
    throw new Error(`Bundled Memix CLI is missing: ${binary}`);
  }
  if (!status.isFile()) {
    throw new Error(`Bundled Memix CLI is not a regular file: ${binary}`);
  }
  return binary;
}

function prependPath(env, directory) {
  const pathKey = Object.keys(env).find((key) => key.toLowerCase() === 'path')
    ?? (process.platform === 'win32' ? 'Path' : 'PATH');
  const current = env[pathKey] ?? process.env[pathKey] ?? process.env.PATH ?? '';
  const normalize = process.platform === 'win32'
    ? (value) => value.toLowerCase()
    : (value) => value;
  const entries = current.split(path.delimiter).filter(Boolean);
  if (!entries.some((entry) => normalize(entry) === normalize(directory))) {
    entries.unshift(directory);
  }
  env[pathKey] = entries.join(path.delimiter);
}

export default {
  id: 'memix',
  setup(ctx) {
    const binary = bundledBinary(ctx.pluginDir);
    const binaryDirectory = path.dirname(binary);
    const registrations = [
      ctx.hooks.on('shell.env', (_input, output) => {
        prependPath(output.env, binaryDirectory);
      }),
      ctx.hooks.on('tool.definition', (input, output) => {
        const redirect = memoryRedirect(input.tool);
        if (redirect) output.description = redirect;
      }),
      ctx.hooks.on(
        'tool.execute.before',
        (input, output) => {
          const redirect = memoryRedirect(input.tool);
          if (!redirect) return;
          output.deny = {
            message: `${redirect} The native OpenDesk memory was not read or changed.`,
            metadata: {
              policy: 'memix-memory-authority',
              replacementSkill: 'pluginMgr/memix-cli',
              nativeTool: input.tool,
              workflow: `memix agent ${nativeMemoryWorkflows.get(input.tool)}`
            }
          };
        },
        { failureMode: 'closed' }
      )
    ];

    return async () => {
      for (const registration of registrations.reverse()) {
        await registration.dispose();
      }
    };
  }
};
