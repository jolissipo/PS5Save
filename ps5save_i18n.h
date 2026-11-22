/* PS5Save+ notification text localisation. The original English messages
 * remain the diagnostic/log language; only Sony ShellUI toast text changes.
 * Unsupported console locales and failed language queries fall back to English.
 */
#ifndef PS5SAVE_PLUS_I18N_H
#define PS5SAVE_PLUS_I18N_H

int sceSystemServiceParamGetInt(int param_id, int *value);

typedef enum {
  PS5_LANG_EN, PS5_LANG_PT, PS5_LANG_PT_BR, PS5_LANG_ES,
  PS5_LANG_FR, PS5_LANG_DE, PS5_LANG_IT
} ps5_language_t;

static ps5_language_t ps5_notification_language(void) {
  int code = 1;
  if(sceSystemServiceParamGetInt(1, &code)) return PS5_LANG_EN;
  switch(code) {
    case 7: return PS5_LANG_PT;
    case 17: return PS5_LANG_PT_BR;
    case 3: case 20: return PS5_LANG_ES;
    case 2: case 22: return PS5_LANG_FR;
    case 4: return PS5_LANG_DE;
    case 5: return PS5_LANG_IT;
    default: return PS5_LANG_EN;
  }
}

/* Prefix entries preserve variable file names, save IDs and byte counts. */
typedef struct {
  const char *original;
  const char *pt, *br, *es, *fr, *de, *it;
  int prefix;
} ps5_translation_t;

static const ps5_translation_t ps5_translations[] = {
  {"Backup up to date. No changes.",
   "Cópia de segurança atualizada. Sem alterações.",
   "Backup atualizado. Nenhuma alteração.",
   "Copia de seguridad al día. Sin cambios.",
   "Sauvegarde à jour. Aucune modification.",
   "Sicherung aktuell. Keine Änderungen.",
   "Backup aggiornato. Nessuna modifica.", 0},
  {"Backup storage moved to ",
   "Cópias de segurança transferidas para ", "Backups movidos para ",
   "Copias trasladadas a ", "Sauvegardes déplacées vers ",
   "Sicherung verschoben nach ", "Backup spostato su ", 1},
  {"Backup protection active.",
   "Cópias de segurança ativas.", "Backup ativo.",
   "Copias de seguridad activas.", "Sauvegardes actives.",
   "Datensicherung aktiv.", "Backup attivo.", 0},
  {"Backup complete (",
   "Cópia de segurança concluída (", "Backup concluído (",
   "Copia de seguridad completada (", "Sauvegarde terminée (",
   "Backup abgeschlossen (", "Backup completato (", 1},
  {"Daily snapshot complete.",
   "Snapshot diário concluído.", "Snapshot diário concluído.",
   "Instantánea diaria completada.", "Instantané quotidien terminé.",
   "Täglicher Snapshot abgeschlossen.", "Snapshot giornaliero completato.", 0},
  {"Backup USB changed. Copying to ",
   "Unidade USB das cópias alterada. A copiar para ",
   "USB dos backups alterado. A copiar para ",
   "Unidad USB de copias cambiada. Copiando a ",
   "USB de sauvegarde modifié. Copie vers ",
   "Backup-USB geändert. Kopiere nach ",
   "Unità USB di backup cambiata. Copia su ", 1},
  {"USB transfer progress: ",
   "Progresso da transferência USB: ",
   "Progresso da transferência USB: ",
   "Progreso de transferencia USB: ",
   "Progression du transfert USB : ",
   "USB-Übertragung: ",
   "Avanzamento trasferimento USB: ", 1},
  {"USB transfer complete.",
   "Transferência USB concluída.",
   "Transferência USB concluída.",
   "Transferencia USB completada.",
   "Transfert USB terminé.",
   "USB-Übertragung abgeschlossen.",
   "Trasferimento USB completato.", 0},
  {"USB transfer failed. See dashboard logs.",
   "Falha na transferência USB. Consulta os registos no dashboard.",
   "Falha na transferência USB. Consulte os registos no dashboard.",
   "Error de transferencia USB. Consulta los registros del panel.",
   "Échec du transfert USB. Consultez les journaux du tableau de bord.",
   "USB-Übertragung fehlgeschlagen. Dashboard-Protokolle prüfen.",
   "Trasferimento USB non riuscito. Controlla i log della dashboard.", 0},
  {"Backup progress: "
   "Progresso da cópia: ", "Progresso do backup: ",
   "Progreso de la copia: ", "Progression de la sauvegarde : ",
   "Backup-Fortschritt: ", "Avanzamento backup: ", 1},
  {"Not enough USB space for backups.",
   "Espaço insuficiente na unidade USB.", "Espaço insuficiente na unidade USB.",
   "No hay espacio suficiente en el USB.", "Espace insuffisant sur la clé USB.",
   "Nicht genügend Speicherplatz auf USB.", "Spazio USB insufficiente.", 0},
  {"Not enough console storage for save backups.",
   "Espaço insuficiente na consola para copiar os saves.",
   "Espaço insuficiente no console para copiar os saves.",
   "Falta espacio en la consola para copiar los datos guardados.",
   "Espace insuffisant sur la console pour les sauvegardes.",
   "Nicht genügend Konsolenspeicher für Spielstände.",
   "Spazio console insufficiente per i salvataggi.", 0},
  {"Backup USB is read-only.",
   "A unidade USB está protegida contra escrita.",
   "A unidade USB está protegida contra gravação.",
   "La unidad USB es de solo lectura.", "La clé USB est en lecture seule.",
   "USB-Laufwerk ist schreibgeschützt.", "Unità USB in sola lettura.", 0},
  {"Daily snapshot could not be saved.",
   "Não foi possível guardar o snapshot diário.",
   "Não foi possível salvar o snapshot diário.",
   "No se pudo guardar la instantánea diaria.",
   "Impossible d'enregistrer l'instantané quotidien.",
   "Täglicher Snapshot konnte nicht gespeichert werden.",
   "Impossibile salvare lo snapshot giornaliero.", 0},
  {"Backup source missing: ",
   "Origem da cópia não encontrada: ", "Origem do backup não encontrada: ",
   "Origen no encontrado: ", "Source introuvable : ",
   "Backup-Quelle nicht gefunden: ", "Origine del backup non trovata: ", 1},
  {"File too large: ",
   "Ficheiro demasiado grande: ", "Arquivo muito grande: ",
   "Archivo demasiado grande: ", "Fichier trop volumineux : ",
   "Datei zu groß: ", "File troppo grande: ", 1},
  {"File backup failed: ",
   "Falha ao copiar o ficheiro: ", "Falha ao copiar o arquivo: ",
   "Error al copiar el archivo: ", "Échec de copie du fichier : ",
   "Datei konnte nicht gesichert werden: ", "Copia del file non riuscita: ", 1},
  {"Backup cannot read: ",
   "Não é possível ler a origem: ", "Não foi possível ler a origem: ",
   "No se puede leer el origen: ", "Source illisible : ",
   "Backup-Quelle nicht lesbar: ", "Impossibile leggere l'origine: ", 1},
  {"Save backup failed (",
   "Falha na cópia do save (", "Falha no backup do save (",
   "Error al copiar los datos guardados (", "Échec de la sauvegarde (",
   "Spielstand-Backup fehlgeschlagen (", "Backup salvataggio non riuscito (", 1},
  {"Invalid save image (",
   "Imagem de save inválida (", "Imagem de save inválida (",
   "Imagen de guardado no válida (", "Image de sauvegarde invalide (",
   "Ungültiges Spielstand-Abbild (", "Immagine salvataggio non valida (", 1},
  {"Too many save images (",
   "Demasiadas imagens de saves (", "Muitas imagens de saves (",
   "Demasiadas imágenes de guardado (", "Trop d'images de sauvegarde (",
   "Zu viele Spielstand-Abbilder (", "Troppe immagini di salvataggio (", 1},
  {"Game saves exceed the size limit (",
   "Os saves ultrapassam o limite de tamanho (",
   "Os saves excedem o limite de tamanho (",
   "Los datos guardados exceden el límite de tamaño (",
   "La taille des sauvegardes dépasse la limite (",
   "Spielstände überschreiten die Größenbegrenzung (",
   "I salvataggi superano il limite di dimensione (", 1},
  {"Game activity is unknown. Save backup postponed.",
   "Não foi possível verificar o jogo. Cópia do save adiada.",
   "Não foi possível verificar o jogo. Backup do save adiado.",
   "Actividad del juego desconocida. Copia aplazada.",
   "Activité du jeu inconnue. Sauvegarde reportée.",
   "Spielaktivität unbekannt. Backup verschoben.",
   "Attività del gioco sconosciuta. Backup rinviato.", 0},
  {"Save backup postponed: unsupported title ID.",
   "Cópia do save adiada: identificador do jogo inválido.",
   "Backup do save adiado: identificador do jogo inválido.",
   "Copia aplazada: identificador de juego no válido.",
   "Sauvegarde reportée : identifiant de jeu non pris en charge.",
   "Backup verschoben: nicht unterstützte Spiel-ID.",
   "Backup rinviato: ID gioco non supportato.", 0},
  {"Save backup paused: cleanup failed.",
   "Cópia de saves suspensa: falha na limpeza temporária.",
   "Backup dos saves pausado: falha na limpeza temporária.",
   "Copia de datos guardados pausada: error de limpieza.",
   "Sauvegarde suspendue : échec du nettoyage.",
   "Spielstand-Backup pausiert: Bereinigung fehlgeschlagen.",
   "Backup salvataggi sospeso: pulizia non riuscita.", 0},
  {"Could not open backup.conf on USB.",
   "Não foi possível abrir backup.conf na unidade USB.",
   "Não foi possível abrir backup.conf no USB.",
   "No se pudo abrir backup.conf en el USB.",
   "Impossible d'ouvrir backup.conf sur la clé USB.",
   "backup.conf auf USB konnte nicht geöffnet werden.",
   "Impossibile aprire backup.conf su USB.", 0},
  {"Could not read backup.conf on USB.",
   "Não foi possível ler backup.conf na unidade USB.",
   "Não foi possível ler backup.conf no USB.",
   "No se pudo leer backup.conf en el USB.",
   "Impossible de lire backup.conf sur la clé USB.",
   "backup.conf auf USB konnte nicht gelesen werden.",
   "Impossibile leggere backup.conf su USB.", 0},
  {"Could not read backup.conf completely.",
   "Não foi possível ler backup.conf por completo.",
   "Não foi possível ler backup.conf completamente.",
   "No se pudo leer backup.conf por completo.",
   "Lecture incomplète de backup.conf.",
   "backup.conf konnte nicht vollständig gelesen werden.",
   "Lettura di backup.conf incompleta.", 0},
  {"Backup could not start: instance lock unavailable.",
   "Não foi possível iniciar: já existe uma instância ou o bloqueio falhou.",
   "Não foi possível iniciar: já existe uma instância ou o bloqueio falhou.",
   "No se pudo iniciar: bloqueo de instancia no disponible.",
   "Démarrage impossible : verrou d'instance indisponible.",
   "Start fehlgeschlagen: Instanzsperre nicht verfügbar.",
   "Avvio fallito: blocco istanza non disponibile.", 0},
  {"Backup could not start: not enough memory.",
   "Não foi possível iniciar: memória insuficiente.",
   "Não foi possível iniciar: memória insuficiente.",
   "No se pudo iniciar: memoria insuficiente.",
   "Démarrage impossible : mémoire insuffisante.",
   "Start fehlgeschlagen: nicht genügend Speicher.",
   "Avvio fallito: memoria insufficiente.", 0},
  {"Backup path is too long.",
   "O caminho da cópia é demasiado longo.",
   "O caminho do backup é muito longo.",
   "La ruta del backup es demasiado larga.",
   "Chemin de sauvegarde trop long.",
   "Backup-Pfad zu lang.",
   "Percorso di backup troppo lungo.", 0},
  {"Backup path too long: ",
   "Caminho demasiado longo: ", "Caminho muito longo: ",
   "Ruta demasiado larga: ", "Chemin trop long : ",
   "Pfad zu lang: ", "Percorso troppo lungo: ", 1},
  {"Backup path too long under ",
   "Caminho demasiado longo dentro de ", "Caminho muito longo dentro de ",
   "Ruta demasiado larga dentro de ", "Chemin trop long sous ",
   "Pfad zu lang unter ", "Percorso troppo lungo sotto ", 1},
  {"Filesystem-root backup refused.",
   "Cópia da raiz do sistema de ficheiros recusada.",
   "Backup da raiz do sistema de arquivos recusado.",
   "No se permite copiar la raíz del sistema de archivos.",
   "Sauvegarde de la racine du système refusée.",
   "Sicherung des Dateisystem-Stammverzeichnisses verweigert.",
   "Backup della radice del file system rifiutato.", 0},
  {"Backup path contains a parent directory (..).",
   "O caminho contém uma referência ao diretório superior (..).",
   "O caminho contém referência ao diretório pai (..).",
   "La ruta contiene un directorio superior (..).",
   "Le chemin contient un dossier parent (..).",
   "Pfad enthält ein übergeordnetes Verzeichnis (..).",
   "Il percorso contiene una directory padre (..).", 0},
  {"Backup destination cannot be a source.",
   "O destino das cópias não pode ser uma origem.",
   "O destino do backup não pode ser uma origem.",
   "El destino del backup no puede ser un origen.",
   "Le dossier de destination ne peut pas être une source.",
   "Backup-Ziel darf keine Quelle sein.",
   "La destinazione del backup non può essere origine.", 0},
  {"Could not save the backup index. Files may be copied again.",
   "Não foi possível guardar o índice. Alguns ficheiros podem ser copiados de novo.",
   "Não foi possível salvar o índice. Alguns arquivos podem ser copiados novamente.",
   "No se pudo guardar el índice. Algunos archivos podrían copiarse de nuevo.",
   "Index non enregistré. Certains fichiers pourront être copiés à nouveau.",
   "Index konnte nicht gespeichert werden. Dateien könnten erneut kopiert werden.",
   "Indice non salvato. Alcuni file potrebbero essere copiati di nuovo.", 0},
  {"Could not check daily snapshot retention.",
   "Não foi possível verificar a retenção dos snapshots.",
   "Não foi possível verificar a retenção dos snapshots.",
   "No se pudo comprobar la retención de instantáneas.",
   "Impossible de vérifier la conservation des instantanés.",
   "Snapshot-Aufbewahrung konnte nicht geprüft werden.",
   "Impossibile verificare la conservazione degli snapshot.", 0},
  {"Could not remove an unfinished snapshot.",
   "Não foi possível remover um snapshot incompleto.",
   "Não foi possível remover um snapshot incompleto.",
   "No se pudo eliminar una instantánea incompleta.",
   "Impossible de supprimer un instantané incomplet.",
   "Unvollständiger Snapshot konnte nicht gelöscht werden.",
   "Impossibile rimuovere uno snapshot incompleto.", 0},
  {"Could not remove an old snapshot.",
   "Não foi possível remover um snapshot antigo.",
   "Não foi possível remover um snapshot antigo.",
   "No se pudo eliminar una instantánea antigua.",
   "Impossible de supprimer un ancien instantané.",
   "Alter Snapshot konnte nicht gelöscht werden.",
   "Impossibile rimuovere uno snapshot precedente.", 0},
  {"Not enough memory to scan saves.",
   "Memória insuficiente para pesquisar os saves.",
   "Memória insuficiente para procurar os saves.",
   "Memoria insuficiente para buscar datos guardados.",
   "Mémoire insuffisante pour analyser les sauvegardes.",
   "Nicht genügend Speicher zum Prüfen der Spielstände.",
   "Memoria insufficiente per analizzare i salvataggi.", 0},
  {"Backup protection restored.",
   "Proteção das cópias de segurança restabelecida.",
   "Proteção dos backups restabelecida.",
   "Protección de copias restablecida.",
   "Protection des sauvegardes rétablie.",
   "Datensicherung wiederhergestellt.",
   "Protezione backup ripristinata.", 0}
};

static const char *ps5_translate_entry(const ps5_translation_t *entry,
                                       ps5_language_t lang) {
  switch(lang) {
    case PS5_LANG_PT: return entry->pt;
    case PS5_LANG_PT_BR: return entry->br;
    case PS5_LANG_ES: return entry->es;
    case PS5_LANG_FR: return entry->fr;
    case PS5_LANG_DE: return entry->de;
    case PS5_LANG_IT: return entry->it;
    default: return entry->original;
  }
}

static const char *ps5_localize_notification(const char *message,
                                             char *translated, size_t cap) {
  ps5_language_t lang = ps5_notification_language();
  if(lang == PS5_LANG_EN) return message;
  for(size_t i = 0; i < sizeof ps5_translations / sizeof ps5_translations[0]; i++) {
    const ps5_translation_t *entry = &ps5_translations[i];
    size_t n = strlen(entry->original);
    if(entry->prefix ? strncmp(message, entry->original, n) :
                       strcmp(message, entry->original)) continue;
    const char *text = ps5_translate_entry(entry, lang);
    int written = snprintf(translated, cap, "%s%s",
                           text, entry->prefix ? message + n : "");
    if(written >= 0 && (size_t)written < cap) return translated;
    return message;
  }
  return message;
}
#endif
