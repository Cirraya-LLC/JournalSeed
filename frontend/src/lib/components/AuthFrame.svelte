<script lang="ts">
  import { BookOpenText } from 'lucide-svelte';

  export let eyebrow: string;
  export let title: string;
  export let subtitle: string;
</script>

<main class="auth-frame">
  <section class="auth-context" aria-label="JournalSeed">
    <div class="brand-mark"><BookOpenText size={24} strokeWidth={1.8} /></div>
    <div class="brand-copy">
      <strong>JournalSeed</strong>
      <span>把每一笔写清楚</span>
    </div>
    <div class="context-copy">
      <p>{eyebrow}</p>
      <h1>{title}</h1>
      <span>{subtitle}</span>
    </div>
    <div class="ledger-lines" aria-hidden="true">
      <i></i><i></i><i></i><i></i><i></i>
    </div>
  </section>
  <section class="auth-form">
    <div class="form-wrap"><slot /></div>
  </section>
</main>

<style>
  .auth-frame {
    display: grid;
    min-height: 100vh;
    min-height: 100dvh;
    grid-template-rows: 220px 1fr;
    background: var(--surface);
  }

  .auth-context {
    position: relative;
    display: flex;
    flex-direction: column;
    overflow: hidden;
    padding: max(24px, env(safe-area-inset-top)) 24px 24px;
    color: oklch(95% 0.012 145);
    background: var(--surface-dark);
  }

  .brand-mark {
    display: grid;
    place-items: center;
    width: 40px;
    height: 40px;
    border: 1px solid oklch(56% 0.035 150);
    border-radius: var(--radius-sm);
    color: var(--accent);
  }

  .brand-copy {
    position: absolute;
    top: max(26px, env(safe-area-inset-top));
    left: 76px;
    display: grid;
    line-height: 1.15;
  }

  .brand-copy strong {
    font-size: 1rem;
  }

  .brand-copy span {
    margin-top: 5px;
    color: oklch(75% 0.025 150);
    font-size: 0.75rem;
  }

  .context-copy {
    position: relative;
    z-index: 1;
    display: grid;
    gap: 4px;
    margin-top: auto;
    max-width: 520px;
  }

  .context-copy p,
  .context-copy h1,
  .context-copy span {
    margin: 0;
  }

  .context-copy p {
    color: var(--accent);
    font-size: 0.75rem;
    font-weight: 700;
  }

  .context-copy h1 {
    font-size: 1.5rem;
    line-height: 1.3;
  }

  .context-copy span {
    color: oklch(76% 0.022 150);
    font-size: 0.875rem;
  }

  .ledger-lines {
    position: absolute;
    right: -24px;
    bottom: 20px;
    display: grid;
    width: 42%;
    gap: 13px;
    transform: rotate(-8deg);
    opacity: 0.22;
  }

  .ledger-lines i {
    height: 1px;
    background: var(--accent);
  }

  .auth-form {
    display: grid;
    align-items: start;
    padding: 32px 24px max(32px, env(safe-area-inset-bottom));
  }

  .form-wrap {
    width: min(100%, 420px);
    margin: 0 auto;
  }

  @media (min-width: 800px) {
    .auth-frame {
      grid-template-columns: minmax(320px, 42%) 1fr;
      grid-template-rows: 1fr;
    }

    .auth-context {
      min-height: 100vh;
      padding: 40px clamp(40px, 6vw, 88px);
    }

    .brand-copy {
      top: 44px;
      left: calc(clamp(40px, 6vw, 88px) + 52px);
    }

    .context-copy {
      gap: 12px;
      margin-block: auto;
    }

    .context-copy h1 {
      max-width: 12ch;
      font-size: 2.25rem;
      line-height: 1.25;
    }

    .context-copy span {
      max-width: 34ch;
      font-size: 1rem;
    }

    .ledger-lines {
      right: -8%;
      bottom: 14%;
      width: 74%;
      gap: 22px;
    }

    .auth-form {
      align-items: center;
      padding: 56px;
    }
  }
</style>
