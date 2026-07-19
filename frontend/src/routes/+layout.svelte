<script lang="ts">
  import { QueryClient, QueryClientProvider } from '@tanstack/svelte-query';
  import '../app.css';

  const queryClient = new QueryClient({
    defaultOptions: {
      queries: {
        staleTime: 20_000,
        retry: 1,
        refetchOnWindowFocus: false
      }
    }
  });
</script>

<QueryClientProvider client={queryClient}>
  <slot />
</QueryClientProvider>
