/* ========================================================================
   MAITRE - STM32F446RE

   Role : le PC envoie des commandes texte au maitre (USART2). Le maitre
   les execute : il pilote des sorties, lit des entrees, ou il envoie des
   trames a l'esclave par le bus RS485 (UART4) et renvoie la reponse au PC.

        PC  --USART2-->  MAITRE  --UART4/RS485-->  ESCLAVE
        PC  <--USART2--  MAITRE  <--UART4/RS485--  ESCLAVE

   Commandes recues du PC (une par ligne) :
     SEQ:<n>      annonce n trames. Elles arrivent ensuite, une par ligne,
                  sous la forme "14 17 27 0F ..." (15 octets en hexa).
     SIG_A_H      met la sortie SIG_A a l'etat haut  (SIG_A_L : etat bas)
                  (idem pour SIG_B ... SIG_F : voir la liste IO_NOM)
     READ_IN      lit les 7 entrees, repond une ligne "IN:0110010"

   Reglages du .ioc :
     UART4  : 1 Mbit/s, Word Length 9 bits, Parity Even
     USART2 : 115200 bauds
     PA15 et les 6 broches de IO_PIN : GPIO_Output Push-Pull
     Les 7 broches de ENTREE_PIN     : GPIO_Input (Pull-up ou Pull-down
                                       selon le materiel teste)
   ======================================================================== */

/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */


/* USER CODE BEGIN PD */

/* ---- Tailles ---- */
#define TRAME_SIZE  15      /* octets envoyes a l'esclave */
#define REPONSE_SIZE 29     /* octets renvoyes par l'esclave */
#define LIGNE_MAX   96      /* taille max d'une ligne recue du PC */
#define TIMEOUT_MS  100     /* attente max de la reponse de l'esclave */

/* ---- Broche DE du LTM2881 (1 = on emet, 0 = on ecoute) ---- */
#define DE_HIGH()   HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_SET)
#define DE_LOW()    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_RESET)

/* ---- Reception de l'UART4 : coupee pendant qu'on emet, sinon le maitre
        recoit ses propres octets ---- */
#define RX_OFF()    (huart4.Instance->CR1 &= ~USART_CR1_RE)
#define RX_ON()     (huart4.Instance->CR1 |=  USART_CR1_RE)

/* ---- Les 6 sorties, toutes sur le port B. Le nom en position i va avec
        la broche en position i (SIG_A = GPIO_PIN_0, SIG_B = GPIO_PIN_1...).
        Ces noms doivent etre identiques a ceux de IO_NOMS dans le script
        python. A adapter a votre montage. ---- */
#define NB_SORTIES  6
static const char *IO_NOM[NB_SORTIES] = {
    "SIG_A", "SIG_B", "SIG_C", "SIG_D", "SIG_E", "SIG_F"
};
static const uint16_t IO_PIN[NB_SORTIES] = {
    GPIO_PIN_0, GPIO_PIN_1, GPIO_PIN_4, GPIO_PIN_5, GPIO_PIN_6, GPIO_PIN_7
};

/* ---- Les 7 entrees a lire, toutes sur le port C (broches d'exemple :
        A REMPLACER par les vraies). L'ordre est celui de IN_NOMS dans le
        script python : la 1re broche = le 1er nom, etc. ---- */
#define NB_ENTREES  7
static const uint16_t ENTREE_PIN[NB_ENTREES] = {
    GPIO_PIN_0, GPIO_PIN_1, GPIO_PIN_2, GPIO_PIN_3,
    GPIO_PIN_4, GPIO_PIN_5, GPIO_PIN_6
};

/* USER CODE END PD */


/* USER CODE BEGIN PV */
static uint8_t reponse[REPONSE_SIZE];   /* derniere reponse de l'esclave */
/* USER CODE END PV */


/* USER CODE BEGIN 0 */

/* ------------------------------------------------------------------------
   PARTIE 1 : dialogue avec le PC
   ------------------------------------------------------------------------ */

/* Envoie un texte au PC */
static void ecrire_au_pc(const char *texte)
{
    HAL_UART_Transmit(&huart2, (uint8_t *)texte, strlen(texte), HAL_MAX_DELAY);
}

/* Attend que le PC envoie une ligne complete (finie par Entree) et la
   recopie dans 'ligne'. */
static void lire_ligne_du_pc(char *ligne, int taille_max)
{
    int nb = 0;         /* nombre de caracteres deja recus */
    uint8_t c;

    while (1)
    {
        HAL_UART_Receive(&huart2, &c, 1, HAL_MAX_DELAY);   /* attend 1 caractere */

        if (c == '\r' || c == '\n')
        {
            if (nb > 0)
            {
                break;                      /* fin de ligne */
            }
            /* sinon : ligne vide (le "\n" qui suit un "\r"), on l'ignore */
        }
        else if (nb < taille_max - 1)
        {
            ligne[nb] = (char)c;
            nb++;
        }
    }
    ligne[nb] = '\0';                       /* fin de chaine */
}

/* Transforme le texte "14 17 27 .." en 15 octets.
   Retourne 1 si ca a marche, 0 si le texte est incorrect. */
static int texte_vers_octets(const char *texte, uint8_t *octets)
{
    int position = 0;   /* ou on en est dans le texte */

    for (int i = 0; i < TRAME_SIZE; i++)
    {
        unsigned int valeur;
        int nb_lus;

        /* lit un nombre en hexa ("%x"), et compte combien de caracteres
           ont ete lus ("%n") pour passer au suivant */
        if (sscanf(texte + position, "%x%n", &valeur, &nb_lus) != 1)
        {
            return 0;
        }
        octets[i] = (uint8_t)valeur;
        position += nb_lus;
    }
    return 1;
}


/* ------------------------------------------------------------------------
   PARTIE 2 : dialogue avec l'esclave (bus RS485)
   ------------------------------------------------------------------------ */

/* Envoie une trame a l'esclave, puis attend sa reponse.
   Retourne 1 si la reponse est arrivee, 0 sinon (timeout). */
static int parler_a_l_esclave(const uint8_t *trame)
{
    /* 1. On emet. Le maitre n'ecoute pas pendant ce temps. */
    RX_OFF();
    DE_HIGH();
    HAL_UART_Transmit(&huart4, (uint8_t *)trame, TRAME_SIZE, TIMEOUT_MS);
    DE_LOW();
    __HAL_UART_CLEAR_OREFLAG(&huart4);
    RX_ON();

    /* 2. On ecoute la reponse */
    return (HAL_UART_Receive(&huart4, reponse, REPONSE_SIZE, TIMEOUT_MS) == HAL_OK);
}


/* ------------------------------------------------------------------------
   PARTIE 3 : les commandes
   ------------------------------------------------------------------------ */

/* SIG_A_H / SIG_A_L ... -> met une sortie a l'etat haut (H) ou bas (L).
   Retourne 1 si le texte est bien une commande de sortie, 0 sinon. */
static int commande_sortie(const char *texte)
{
    int longueur = (int)strlen(texte);

    /* le texte doit finir par "_H" ou "_L" */
    if (longueur < 3 || texte[longueur - 2] != '_')
    {
        return 0;
    }
    char niveau = texte[longueur - 1];
    if (niveau != 'H' && niveau != 'L')
    {
        return 0;
    }

    /* cherche le nom (le texte sans le "_H" / "_L") dans la liste */
    for (int i = 0; i < NB_SORTIES; i++)
    {
        if ((int)strlen(IO_NOM[i]) == longueur - 2 &&
            strncmp(texte, IO_NOM[i], longueur - 2) == 0)
        {
            int etat = (niveau == 'H');
            char message[40];

            HAL_GPIO_WritePin(GPIOB, IO_PIN[i], etat ? GPIO_PIN_SET : GPIO_PIN_RESET);
            sprintf(message, "%s = %d\r\n", IO_NOM[i], etat);
            ecrire_au_pc(message);
            return 1;
        }
    }
    return 0;
}

/* READ_IN -> lit les 7 entrees et repond "IN:0110010"
   (un chiffre par entree, dans l'ordre de ENTREE_PIN) */
static void commande_lecture_entrees(void)
{
    char message[16] = "IN:";

    for (int i = 0; i < NB_ENTREES; i++)
    {
        int etat = (HAL_GPIO_ReadPin(GPIOC, ENTREE_PIN[i]) == GPIO_PIN_SET);
        message[3 + i] = etat ? '1' : '0';
    }
    message[3 + NB_ENTREES] = '\0';
    strcat(message, "\r\n");
    ecrire_au_pc(message);
}

/* SEQ:<n> -> recoit n trames du PC, les envoie une par une a l'esclave,
   et renvoie chaque reponse au PC. S'arrete a la premiere erreur. */
static void commande_sequence(int nb_trames)
{
    char ligne[LIGNE_MAX];
    char message[160];
    uint8_t trame[TRAME_SIZE];

    for (int numero = 1; numero <= nb_trames; numero++)
    {
        /* a. Le PC nous envoie la trame sous forme de texte */
        lire_ligne_du_pc(ligne, sizeof(ligne));

        if (!texte_vers_octets(ligne, trame))
        {
            sprintf(message, "Trame %02d : format invalide\r\n", numero);
            ecrire_au_pc(message);
            break;
        }

        /* b. On l'envoie a l'esclave et on attend sa reponse */
        if (!parler_a_l_esclave(trame))
        {
            sprintf(message, "Trame %02d : TIMEOUT (pas d'echo)\r\n", numero);
            ecrire_au_pc(message);
            break;
        }

        /* c. On renvoie la reponse au PC : "Trame 01 : XX XX XX .." */
        int taille = sprintf(message, "Trame %02d : ", numero);
        for (int i = 0; i < REPONSE_SIZE; i++)
        {
            taille += sprintf(message + taille, "%02X ", reponse[i]);
        }
        strcat(message, "\r\n");
        ecrire_au_pc(message);
    }

    ecrire_au_pc("Sequence terminee.\r\n");
}

/* Regarde quelle commande le PC a envoyee, et l'execute */
static void executer_commande(const char *texte)
{
    int n;

    if (sscanf(texte, "SEQ:%d", &n) == 1)
    {
        commande_sequence(n);
    }
    else if (strcmp(texte, "READ_IN") == 0)
    {
        commande_lecture_entrees();
    }
    else if (commande_sortie(texte))
    {
        /* deja traitee (et le PC a deja recu la reponse) */
    }
    else
    {
        ecrire_au_pc("Commande inconnue\r\n");
    }
}

/* USER CODE END 0 */


/* USER CODE BEGIN 2 */
DE_LOW();                               /* au demarrage : on ecoute */
ecrire_au_pc("Maitre pret.\r\n");
/* USER CODE END 2 */


/* USER CODE BEGIN WHILE */
while (1)
{
    /* Boucle principale : attendre une commande du PC, puis l'executer */
    char commande[LIGNE_MAX];
    lire_ligne_du_pc(commande, sizeof(commande));
    executer_commande(commande);
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
}
/* USER CODE END 3 */
