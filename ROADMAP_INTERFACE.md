# 🎨 SwitchCast — Master Roadmap & Guia de Design System de Interface (Padrão Vitral Stealth)

> **Versão da Interface**: v2.1.0 | Comp. 24  
> **Objetivo**: Especificação canônica e matemática do Design System do SwitchCast (Camada 3D, Vitral Joy-Con, Micro-símbolos com Sombra Preta e Delineados Sólidos Escuros) para implementação direta pelo **Gemini Flash**.

---

## 📐 1. Princípios e Proporções Matemáticas

O layout é composto por **3 planos ópticos sobrepostos** em profundidade (Z-index):

```
┌────────────────────────────────────────────────────────────────────────┐
│ PLANO 1 (Traseiro): Tela Base 90% Preto / 10% Cinza (#0D0E12)         │
│   ┌────────────────────────────────────────────────────────────────┐   │
│   │ PLANO 2 (Intermediário): Vitrais Neon Dual + PlayStation Mini  │   │
│   │   ┌────────────────────────────────────────────────────────┐   │   │
│   │   │ PLANO 3 (Frontal): Tela 2 (78% escala, borda #383D48)  │   │   │
│   │   │   • Todos os botões e textos em 1 LINHA ESTRITA        │   │   │
│   │   │   • Sombras pretas profundas em todos os itens         │   │   │
│   │   │   • Tipografia Segoe UI Variable / Inter ampliada      │   │   │
│   │   └────────────────────────────────────────────────────────┘   │   │
│   └────────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────────────┘
```

---

## 🎨 2. Paleta de Cores e Tokens Oficiais (100% Sólidos e Contrastes)

| Token / Elemento | Valor Hex / Função | Aplicação / Regra |
| :--- | :--- | :--- |
| **Fundo Tela Base (Plano 1)** | `#0D0E12` (`color-mix: 90% #000000, 10% #707075`) | OLED Black profundo sem banding |
| **Borda Base Traseira** | `#242730` (Sólido) | Contorno exterior escuro de 1px |
| **Borda Tela 2 (Plano 3)** | `#383D48` (Sólido) | Contorno titânio/grafite escuro de 1.5px |
| **Borda Interna Tela 2** | `#1A1D24` (Sólido) | Anel interno de 1px para profundidade |
| **Fundo Tela 2 (Vidro)** | `rgba(14, 16, 21, 0.85)` + Blur 36px | Superfície acrílica translúcida escura |
| **Switch Joy-Con (L)** | `#00C3E3` (Neon Blue) | Vitral esquerdo e acento do controle esquerdo |
| **Borda Sólida Joy-Con (L)** | `#0A637A` (Azul Petróleo Escuro Sólido) | Delineado do Card L e Badge Neon L |
| **Switch Joy-Con (R)** | `#FF3C28` (Neon Red) | Vitral direito e acento do controle direito |
| **Borda Sólida Joy-Con (R)** | `#7A1D16` (Carmesim Escuro Sólido) | Delineado do Card R e Badge Neon R |
| **Borda Sólida Neutra** | `#383D48` (Sólido) | Borda do Card Central, Botão Shader e Badge Online |
| **Sombras Pretas (Geral)** | `#000000` (Opacidade 0.90 a 1.0) | Presente em TODOS os itens (caixas, textos e ícones) |

---

## 🧩 3. As 3 Camadas de Renderização (Especificação de Código)

### Camada 1: A Tela Base (90% Preto, 10% Cinza)
* **Dimensão**: Container com proporção de tela de console (ex.: `680px x 410px` em web ou `1280x720` / `1920x1080` nativo).
* **Borda**: `1px solid #242730` (cor sólida escura).
* **Sombra de Projeção**: `box-shadow: 0 28px 70px #000000;`.
* **Cantos**: Raio de curvatura de `24px` a `28px`.

---

### Camada 2: O Vitral Bicolor + Micro-símbolos PlayStation
Localizada entre a Tela Base e a Tela Frontal:

1. **Facetas do Vitral (Losangos com Blur)**:
   * **Vitral L (Esquerda - Neon Blue)**:
     * Forma: Polígono em losango `clip-path: polygon(50% 0%, 100% 50%, 50% 100%, 0% 50%)`.
     * Gradiente: `from-[#00C3E3]/40 via-[#00607A]/25 to-transparent`.
     * Desfoque & Sombra: `filter: blur(20px) drop-shadow(0 8px 18px #000000)`.
   * **Vitral R (Direita - Neon Red)**:
     * Forma: Polígono em losango idêntico.
     * Gradiente: `from-[#FF3C28]/40 via-[#7A140A]/25 to-transparent`.
     * Desfoque & Sombra: `filter: blur(22px) drop-shadow(0 8px 18px #000000)`.
   * **Centro**: Faceta poligonal de fusão com `blur(30px)`.

2. **Micro-símbolos PlayStation (Textura Tátil DualSense)**:
   * **Símbolos**: Losangos (◇), Triângulos (△), Círculos (○) e Quadrados (□).
   * **Regra Fundamental**: **TODOS os símbolos devem conter sombra preta** (`feDropShadow` em SVG ou `drop-shadow(0 3px 6px #000000)` em CSS/GLSL) com deslocamento `dx="2" dy="2.5"` e opacidade `1.0`.

---

### Camada 3: Tela Frontal (Compacta a 78%, Bordas Sólidas Escuras)
* **Escala**: Ocupa exatamente **78%** de largura (limite máximo `480px` em cards de diálogo).
* **Delineado de Borda**: **1.5px solid #383D48** (cor sólida em grafite escuro, NUNCA translúcida).
* **Superfície**: `rgba(14, 16, 21, 0.85)` com `backdrop-filter: blur(38px) saturate(180%)`.
* **Sombras de Caixa**: `box-shadow: 0 34px 68px -10px #000000, 0 16px 30px -6px rgba(0, 0, 0, 0.98);`.

---

## 🔤 4. Tipografia & Regra de Ouro: "1 Linha Estrita"

### Tipografia
* **Pilha de Fontes**:
  ```css
  font-family: "Segoe UI Variable Display", "Segoe UI Variable Text", "Segoe UI", "Inter", -apple-system, BlinkMacSystemFont, sans-serif;
  font-feature-settings: "cv05", "cv08", "ss01";
  ```
* **Hierarquia e Sombras de Texto**:
  * **Título Principal**: `17px`, peso `650`, sombra `text-shadow: 0 2px 4px #000000, 0 4px 8px rgba(0,0,0,0.95);`.
  * **Métricas e Valores**: `14px`, negrito (`700`), cor correspondente ao Neon ou Branco com sombra preta.
  * **Subtítulos e Legendas**: `12.5px`, peso normal, cor `#A1A1AA` com sombra preta suave.

### Regra de Ouro: Proibido Quebrar em 2 Linhas
* **Cabeçalho**:
  * Ícone, Nome da aplicação, Chips `Neon L` / `Neon R` e Badge `Online` devem estar alinhados horizontalmente (`items-center`) e com `whitespace-nowrap flex-nowrap`.
* **Cards de Métricas**:
  * Altura fixa padronizada (`h-16`) com textos centralizados e sem quebras internas.
* **Rodapé de Ações**:
  * Os botões Joy-Con `[L]` / `[R]` e o botão de ação (ex.: `Shader`) devem ter altura idêntica (`h-8`), alinhamento na mesma linha horizontal central e `whitespace-nowrap`.

---

## 💻 5. Guia de Implementação Rápida para o Gemini Flash

### Implementação A: Para o Web Remote HTML/CSS (`src/network/WebRemote.hpp`)
Usar as seguintes classes e estilos inline testados e validados:
```html
<!-- Exemplo Canônico do Card Frontal -->
<div class="front-panel w-[78%] max-w-[480px] p-6 rounded-[24px] flex flex-col justify-between gap-4 text-white"
     style="background: rgba(14,16,21,0.85); border: 1.5px solid #383D48; backdrop-filter: blur(38px);">
  
  <!-- Linha do Cabeçalho -->
  <div class="flex items-center justify-between gap-3 w-full flex-nowrap whitespace-nowrap">
    <div class="flex items-center gap-3 min-w-0">
      <div class="w-10 h-10 rounded-xl flex items-center justify-center border border-[#383D48]"
           style="background: linear-gradient(135deg, rgba(0,195,227,0.2), rgba(255,60,40,0.2)); box-shadow: 0 4px 12px #000000;">
        <!-- Ícone -->
      </div>
      <div class="flex items-center gap-2">
        <span class="text-[17px] font-semibold text-zinc-200" style="text-shadow: 0 2px 4px #000000;">SwitchCast</span>
        <span class="text-[11px] font-bold px-2 py-0.5 rounded-full text-[#00C3E3]" style="border: 1px solid #0A637A; background: #000000;">Neon L</span>
        <span class="text-[11px] font-bold px-2 py-0.5 rounded-full text-[#FF3C28]" style="border: 1px solid #7A1D16; background: #000000;">Neon R</span>
      </div>
    </div>
    <div class="h-8 flex items-center gap-2 text-zinc-300 text-xs px-3 rounded-full border border-[#383D48]" style="background: rgba(0,0,0,0.9); box-shadow: 0 4px 12px #000000;">
      <span class="w-2 h-2 rounded-full bg-[#00C3E3]"></span>
      <span>Online</span>
    </div>
  </div>

  <!-- Métricas em 3 Colunas com Bordas Sólidas Escuras -->
  <div class="grid grid-cols-3 gap-2.5 w-full">
    <div class="h-16 rounded-xl flex flex-col items-center justify-center" style="background: rgba(0,0,0,0.75); border: 1px solid #0A637A; box-shadow: 0 8px 18px #000000;">
      <span class="text-xs text-zinc-400">Joy-Con L</span>
      <span class="text-xs font-bold text-[#00C3E3]">#00C3E3</span>
    </div>
    <div class="h-16 rounded-xl flex flex-col items-center justify-center" style="background: rgba(0,0,0,0.75); border: 1px solid #383D48; box-shadow: 0 8px 18px #000000;">
      <span class="text-xs text-zinc-400">Escala</span>
      <span class="text-xs font-bold text-zinc-300">Compacta</span>
    </div>
    <div class="h-16 rounded-xl flex flex-col items-center justify-center" style="background: rgba(0,0,0,0.75); border: 1px solid #7A1D16; box-shadow: 0 8px 18px #000000;">
      <span class="text-xs text-zinc-400">Joy-Con R</span>
      <span class="text-xs font-bold text-[#FF3C28]">#FF3C28</span>
    </div>
  </div>

  <!-- Rodapé em Linha Única Estrita -->
  <div class="flex items-center justify-between gap-3 w-full pt-3 border-t border-[#2A2E38] flex-nowrap whitespace-nowrap">
    <div class="flex items-center gap-2 text-zinc-300 text-xs">
      <span class="w-5 h-5 flex items-center justify-center rounded-md text-[#00C3E3] font-bold text-[11px]" style="background: #000000; border: 1px solid #0A637A;">L</span>
      <span>Delineado Sólido Dark</span>
      <span class="w-5 h-5 flex items-center justify-center rounded-md text-[#FF3C28] font-bold text-[11px]" style="background: #000000; border: 1px solid #7A1D16;">R</span>
    </div>
    <button class="h-8 px-4 rounded-xl text-zinc-200 font-bold text-xs" style="background: rgba(0,0,0,0.85); border: 1px solid #383D48; box-shadow: 0 4px 12px #000000;">
      Shader
    </button>
  </div>
</div>
```

---

### Implementação B: Para o Renderizador Nativo OpenGL 4.3 (`src/ui/Renderer2D.cpp`)
No C++ nativo do Nintendo Switch, desenhar os elementos na seguinte ordem de chamadas do `Renderer2D`:
1. `DrawRect(0, 0, width, height, Color::Hex(0x0D0E12));` // Fundo 90/10
2. `DrawShadedPolygon(...)` com shaders de fragmento com desfoque gaussiano em `#00C3E3` (esquerda) e `#FF3C28` (direita).
3. `DrawTexture(...)` com a textura de micro-símbolos geométricos contendo sombra pré-renderizada preta.
4. `DrawRoundedRect(cardX, cardY, cardW, cardH, 24.0f, Color::Rgba(14, 16, 21, 215));`
5. `DrawRoundedRectOutline(cardX, cardY, cardW, cardH, 24.0f, 1.5f, Color::Hex(0x383D48));` // Borda sólida escura
6. `DrawText(...)` com offsets para sombra preta desenhada 1px abaixo e à direita antes do texto principal.

---

## ✅ Checklist de Validação para o Flash
- [x] O fundo é 90% preto e 10% cinza (`#0D0E12`)?
- [x] O vitral usa os tons Neon Blue (`#00C3E3`) e Neon Red (`#FF3C28`) do Switch?
- [x] Os micro-símbolos PlayStation possuem sombras pretas marcadas?
- [x] A Tela 2 está na proporção compacta de 78%?
- [x] Os delineados de borda são em cores sólidas escuras (`#383D48`, `#0A637A`, `#7A1D16`) sem transparência?
- [x] Todos os botões, chips e rodapés estão estritamente em 1 linha sem quebra?
- [x] Todos os itens possuem sombras pretas profundas?
- [x] A fonte principal é Segoe UI Variable com fallback Inter?
