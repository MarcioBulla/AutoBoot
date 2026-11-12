<script setup lang="ts">
import { ref } from 'vue';

const count = ref(0);
</script>

<template>
  <div class="counter">
    <button type="button" @click="count++">
      Vue 3 ativo · cliques: {{ count }}
    </button>
  </div>
</template>

<style scoped>
.counter button {
  border: 1px solid #4765d7;
  border-radius: 0.75rem;
  background: #4765d7;
  color: white;
  cursor: pointer;
  font: inherit;
  font-weight: 650;
  padding: 0.85rem 1.15rem;
  transition: transform 160ms ease, background-color 160ms ease;
}

.counter button:hover {
  background: #354eb3;
  transform: translateY(-2px);
}

.counter button:focus-visible {
  outline: 3px solid #a7b6ff;
  outline-offset: 3px;
}
</style>
